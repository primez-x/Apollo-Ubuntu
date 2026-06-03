/**
 * @file src/platform/linux/pipewiregrab.cpp
 * @brief GNOME Mutter ScreenCast/PipeWire capture.
 */

#ifdef SUNSHINE_BUILD_PIPEWIRE

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <unistd.h>

// local includes
#include "graphics.h"
#include "cuda.h"
#include "mutter_dbus.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "virtual_display.h"

using namespace std::literals;

namespace platf {
  namespace {
	    bool mutter_screencast_available() {
      GError *raw_error = nullptr;
      auto bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &raw_error);
      mutter_dbus::gerror_ptr error(raw_error);
      if (!bus) {
        return false;
      }

      const bool has_owner = mutter_dbus::name_has_owner(
        bus,
        mutter_dbus::SCREENCAST_SERVICE,
        mutter_dbus::QUICK_CALL_TIMEOUT_MS
      );
      g_object_unref(bus);
	      return has_owner;
	    }

      enum class pipewire_capture_mode_e {
        MAPPED,
        DMABUF
      };

      const char *pipewire_capture_mode_name(pipewire_capture_mode_e mode) {
        return mode == pipewire_capture_mode_e::DMABUF ? "dmabuf" : "mapped";
      }

      // Process-wide sticky DMA-BUF disable. Set once an AUTO-policy DMA-BUF import
      // fails at runtime so the subsequent capture_e::reinit (which destroys and
      // recreates the display + encode device) comes back on the mapped path and
      // never re-attempts DMA-BUF — guaranteeing the fallback happens at most once.
      std::atomic<bool> g_pipewire_dmabuf_disabled {false};

      /**
       * @brief Map a DRM primary node (cardN) to its render node path.
       *
       * Reads /sys/class/drm/<card>/device/drm/ and returns the first renderD*
       * entry mapped to /dev/dri/renderDXXX. Returns empty if the card has no
       * render node (e.g. a display-only KMS device).
       */
      std::string render_node_for_card(const std::string &card) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path drm_dir = fs::path("/sys/class/drm") / card / "device" / "drm";
        for (const auto &entry : fs::directory_iterator(drm_dir, ec)) {
          if (ec) {
            break;
          }
          const auto name = entry.path().filename().string();
          if (name.rfind("renderD", 0) == 0) {
            return std::string("/dev/dri/") + name;
          }
        }
        return {};
      }

      /**
       * @brief Read the kernel driver name bound to a DRM card (e.g. "nvidia", "evdi").
       */
      std::string driver_for_card(const std::string &card) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path driver_link = fs::path("/sys/class/drm") / card / "device" / "driver";
        const fs::path target = fs::read_symlink(driver_link, ec);
        if (ec) {
          return {};
        }
        return target.filename().string();
      }

      /**
       * @brief Resolve the DRM render node the DMA-BUF encode/import path should use.
       *
       * The zero-copy DMA-BUF import only works when the NVENC encode GPU matches
       * the GPU GNOME composites on (the GPU driving the physical display). This:
       *   - honours config::video.adapter_name verbatim if set (manual override), else
       *   - auto-detects the compositor GPU by scanning /sys/class/drm for a connected,
       *     non-virtual connector (skips "Meta-*" virtual heads and "evdi" cards) and
       *     maps that card to its render node.
       *
       * @param manual_override true if the returned path came from adapter_name.
       * @return The /dev/dri/renderDXXX path, or empty when nothing suitable is found.
       */
      std::string resolve_dmabuf_encode_render_node(bool &manual_override) {
        manual_override = false;

        if (!config::video.adapter_name.empty()) {
          manual_override = true;
          return config::video.adapter_name;
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator("/sys/class/drm", ec)) {
          if (ec) {
            break;
          }
          if (!entry.is_directory()) {
            continue;
          }
          const auto dir_name = entry.path().filename().string();

          // Connector dirs look like "cardN-<connector>" (e.g. card3-DP-6).
          if (dir_name.rfind("card", 0) != 0) {
            continue;
          }
          const auto dash = dir_name.find('-');
          if (dash == std::string::npos) {
            continue;  // bare "cardN" entry, not a connector
          }

          const std::string card = dir_name.substr(0, dash);
          const std::string connector = dir_name.substr(dash + 1);

          // Skip Mutter's virtual head; we want the GPU GNOME actually composites on.
          if (connector.rfind("Meta-", 0) == 0) {
            continue;
          }

          // Only consider physically connected connectors.
          std::ifstream status_file(entry.path() / "status");
          std::string status;
          if (!(status_file >> status) || status != "connected") {
            continue;
          }

          // Skip virtual EVDI cards (Apollo's own virtual display path).
          if (driver_for_card(card) == "evdi") {
            continue;
          }

          auto render_node = render_node_for_card(card);
          if (!render_node.empty()) {
            return render_node;
          }
        }

        return {};
      }

      constexpr std::uint32_t GAMESCOPE_FORMAT_REQUESTED_SIZE = 0x70000;
      constexpr std::uint32_t GAMESCOPE_FORMAT_FOCUS_APPID = 0x70001;
      constexpr const char *GAMESCOPE_PIPEWIRE_TARGET = "gamescope";

      constexpr std::array<const char *, 24> SOFTWARE_CURSOR_BITMAP {{
        "X...............",
        "XX..............",
        "XWX.............",
        "XWWX............",
        "XWWWX...........",
        "XWWWWX..........",
        "XWWWWWX.........",
        "XWWWWWWX........",
        "XWWWWWWWX.......",
        "XWWWWWWWWX......",
        "XWWWWWWWWWX.....",
        "XWWWWWWWWWWX....",
        "XWWWWWWWWWWWX...",
        "XWWWWWWWWWWWWX..",
        "XWWWWWWXXXXXXX..",
        "XWWWXWWX........",
        "XWWX.XWWX.......",
        "XWX..XWWX.......",
        "XX...XWWWX......",
        "X.....XWWWX.....",
        "......XWWWX.....",
        ".......XWWWX....",
        ".......XWWWX....",
        "........XXX.....",
      }};

      void blend_software_cursor(img_t &img, const VDISPLAY::gamescope_cursor_state_t &cursor) {
        if (!img.data || img.pixel_pitch < 4 || img.row_pitch <= 0 || img.width <= 0 || img.height <= 0 || !cursor.visible) {
          return;
        }

        const auto cursor_x = static_cast<int>(cursor.x);
        const auto cursor_y = static_cast<int>(cursor.y);
        for (std::size_t y = 0; y < SOFTWARE_CURSOR_BITMAP.size(); ++y) {
          const auto dst_y = cursor_y + static_cast<int>(y);
          if (dst_y < 0 || dst_y >= img.height) {
            continue;
          }

          const auto *row = SOFTWARE_CURSOR_BITMAP[y];
          for (int x = 0; row[x] != '\0'; ++x) {
            const auto pixel = row[x];
            if (pixel == '.') {
              continue;
            }

            const auto dst_x = cursor_x + x;
            if (dst_x < 0 || dst_x >= img.width) {
              continue;
            }

            auto dst = img.data + (static_cast<std::size_t>(dst_y) * img.row_pitch) + (static_cast<std::size_t>(dst_x) * img.pixel_pitch);
            const auto value = static_cast<std::uint8_t>(pixel == 'W' ? 255 : 0);
            dst[0] = value;
            dst[1] = value;
            dst[2] = value;
            dst[3] = 0xFF;
          }
        }
      }

      std::optional<std::uint32_t> pipewire_format_to_drm_fourcc(spa_video_format format) {
        switch (format) {
          case SPA_VIDEO_FORMAT_BGRx:
            return DRM_FORMAT_XRGB8888;
          case SPA_VIDEO_FORMAT_BGRA:
            return DRM_FORMAT_ARGB8888;
          case SPA_VIDEO_FORMAT_RGBx:
            return DRM_FORMAT_XBGR8888;
          case SPA_VIDEO_FORMAT_RGBA:
            return DRM_FORMAT_ABGR8888;
          default:
            return std::nullopt;
        }
      }

      void close_surface_fds(egl::surface_descriptor_t &sd) {
        for (auto &fd : sd.fds) {
          if (fd >= 0) {
            close(fd);
            fd = -1;
          }
        }
      }

      struct dmabuf_frame_t {
        dmabuf_frame_t() {
          std::fill(std::begin(sd.fds), std::end(sd.fds), -1);
        }

        dmabuf_frame_t(const dmabuf_frame_t &) = delete;
        dmabuf_frame_t &operator=(const dmabuf_frame_t &) = delete;

        dmabuf_frame_t(dmabuf_frame_t &&other) noexcept {
          move_from(std::move(other));
        }

        dmabuf_frame_t &operator=(dmabuf_frame_t &&other) noexcept {
          if (this != &other) {
            reset();
            move_from(std::move(other));
          }
          return *this;
        }

        ~dmabuf_frame_t() {
          reset();
        }

        void reset() {
          close_surface_fds(sd);
          valid = false;
        }

        void move_to(egl::img_descriptor_t &img) {
          img.reset();
          img.sd = sd;
          std::fill(std::begin(sd.fds), std::end(sd.fds), -1);
          img.frame_timestamp = timestamp;
          img.data = nullptr;
          valid = false;
        }

        void move_from(dmabuf_frame_t &&other) {
          sd = other.sd;
          timestamp = other.timestamp;
          valid = other.valid;
          pw_buf = other.pw_buf;
          std::fill(std::begin(other.sd.fds), std::end(other.sd.fds), -1);
          other.valid = false;
          other.pw_buf = nullptr;
        }

        egl::surface_descriptor_t sd {};
        std::chrono::steady_clock::time_point timestamp {};
        bool valid {};
        // The PipeWire buffer this frame was captured from. Ownership of the
        // buffer (i.e. responsibility to pw_stream_queue_buffer it) travels with
        // the frame; it is NOT released by reset()/move_to(). The consumer must
        // re-queue it only once the encoder has finished importing the frame.
        struct pw_buffer *pw_buf {};
      };

      // DMA-BUF capture image that remembers which PipeWire buffer backs it. The
      // buffer stays checked out of PipeWire's pool until this image is handed
      // back to the free pool (use_count()==1), which only happens after the
      // encoder has imported the frame in convert(). That keeps the compositor
      // from rendering new content into a buffer we are still encoding from --
      // the cause of the "last few frames loop" artifact on static screens.
      class pw_dmabuf_img_t: public egl::img_descriptor_t {
      public:
        struct pw_buffer *pw_buf {};
      };

	    class pipewire_img_t: public img_t {
    public:
      std::vector<std::uint8_t> storage;
    };

    class pipewire_display_t: public display_t {
    public:
      ~pipewire_display_t() override {
        stop();
      }

	      int init(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
	        this->display_name = display_name;
	        mem_type = hwdevice_type;
	        width = config.width;
	        height = config.height;
        const auto virtual_backend = VDISPLAY::virtualDisplayBackend(display_name);
        gamescope_pipewire_capture = virtual_backend == VDISPLAY::BACKEND::GAMESCOPE_PIPEWIRE;
        is_mutter_pipewire = virtual_backend == VDISPLAY::BACKEND::MUTTER_PIPEWIRE;
	        configure_dmabuf_policy();
	        std::uint32_t requested_framerate_override {};
        std::uint32_t virtual_width {};
        std::uint32_t virtual_height {};
        std::uint32_t virtual_fps {};
        if ((virtual_backend == VDISPLAY::BACKEND::MUTTER_PIPEWIRE ||
             virtual_backend == VDISPLAY::BACKEND::GAMESCOPE_PIPEWIRE) &&
            VDISPLAY::getVirtualDisplayMode(display_name, virtual_width, virtual_height, virtual_fps)) {
          width = static_cast<int>(virtual_width);
          height = static_cast<int>(virtual_height);
          if (virtual_fps) {
            requested_framerate_override = virtual_fps;
          }
        }
        env_width = width;
        env_height = height;
	        auto requested_framerate = requested_framerate_override ? requested_framerate_override : config.framerate;
	        if (requested_framerate >= 1000) {
	          requested_framerate /= 1000;
	        }
	        framerate = std::max<std::uint32_t>(1, requested_framerate);
	        frame_interval = std::chrono::nanoseconds {1s} / framerate;

        if (!start_mutter_stream()) {
          return -1;
        }

        if (!start_pipewire_stream()) {
          return -1;
        }

        return 0;
	      }

		      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
	        if (active_capture_mode == pipewire_capture_mode_e::DMABUF) {
	          return capture_dmabuf(push_captured_image_cb, pull_free_image_cb);
	        }

	        return capture_mapped(push_captured_image_cb, pull_free_image_cb, cursor && *cursor);
	      }

	      capture_e capture_mapped(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool cursor_enabled) {
		        capture_diag_at = std::chrono::steady_clock::now();
		        capture_frames = 0;
	        capture_new_frames = 0;
	        capture_repeated_frames = 0;
	        capture_max_wait_ms = 0;
	        capture_max_copy_ms = 0;

	        while (running) {
	          std::shared_ptr<img_t> img;
	          if (!pull_free_image_cb(img)) {
	            return capture_e::interrupted;
	          }

	          bool copied = false;
	          double wait_ms = 0;
	          double copy_ms = 0;
	          {
	            std::unique_lock lock(frame_mutex);
	            auto wanted_generation = consumed_generation;
	            lock.unlock();
	            auto wait_start = std::chrono::steady_clock::now();
	            trigger_pipewire_process();
	            lock.lock();
	            frame_cv.wait_for(lock, frame_interval, [&]() {
	              return !running || latest_generation != wanted_generation;
	            });
	            wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();

	            if (!running) {
	              return capture_e::interrupted;
	            }

	            VDISPLAY::gamescope_cursor_state_t cursor_state;
	            const bool should_overlay_cursor =
	              gamescope_pipewire_capture &&
	              cursor_enabled &&
	              VDISPLAY::getGamescopeCursorState(display_name, cursor_state) &&
	              cursor_state.visible;
	            const bool frame_changed = latest_generation != consumed_generation;
	            const bool cursor_changed = should_overlay_cursor && cursor_state.serial != consumed_cursor_serial;

	            if (!latest_pixels.empty() && (frame_changed || cursor_changed)) {
	              auto copy_start = std::chrono::steady_clock::now();
	              const auto copy_height = std::min<int>(height, latest_height);
	              const auto copy_width = std::min<int>(width, latest_width);
	              for (int y = 0; y < copy_height; ++y) {
	                auto src = latest_pixels.data() + (static_cast<std::size_t>(y) * latest_stride);
	                auto dst = img->data + (static_cast<std::size_t>(y) * img->row_pitch);
	                std::memcpy(dst, src, static_cast<std::size_t>(copy_width) * 4);
	              }
	              if (should_overlay_cursor) {
	                if (!logged_software_cursor_overlay) {
	                  logged_software_cursor_overlay = true;
	                  BOOST_LOG(info) << "Gamescope PipeWire software cursor overlay is active for " << display_name << '.';
	                }
	                blend_software_cursor(*img, cursor_state);
	                consumed_cursor_serial = cursor_state.serial;
	              }
	              copy_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copy_start).count();
	              img->frame_timestamp = frame_changed ? latest_timestamp : std::chrono::steady_clock::now();
	              consumed_generation = latest_generation;
	              copied = true;
	            }
	          }

	          log_capture_diag(copied, wait_ms, copy_ms);

	          if (!push_captured_image_cb(std::move(img), copied)) {
	            return capture_e::ok;
	          }
        }

        return capture_e::interrupted;
	      }

	      capture_e capture_dmabuf(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb) {
	        capture_diag_at = std::chrono::steady_clock::now();
	        capture_frames = 0;
	        capture_new_frames = 0;
	        capture_repeated_frames = 0;
	        capture_max_wait_ms = 0;
	        capture_max_copy_ms = 0;

	        while (running) {
	          // AUTO graceful fallback: a runtime DMA-BUF import failed. Reinitialize so
	          // the video core rebuilds this session on the mapped path (the encode device
	          // differs between paths, so it must be rebuilt). g_pipewire_dmabuf_disabled
	          // stays set process-wide, so the rebuild does not re-attempt DMA-BUF.
	          if (request_mapped_reinit_.load()) {
	            return capture_e::reinit;
	          }

	          std::shared_ptr<img_t> img;
	          if (!pull_free_image_cb(img)) {
	            return capture_e::interrupted;
	          }

	          // The pool only returns an image once the encoder is done with its prior
	          // frame (use_count()==1). Any PipeWire buffer that image still holds from
	          // an earlier capture is therefore safe to return to the pool now.
	          auto *dmabuf_img = static_cast<pw_dmabuf_img_t *>(img.get());
	          if (dmabuf_img->pw_buf) {
	            requeue_buffer_from_consumer(dmabuf_img->pw_buf);
	            dmabuf_img->pw_buf = nullptr;
	          }

	          bool captured = false;
	          double wait_ms = 0;
	          {
	            std::unique_lock lock(frame_mutex);
	            auto wanted_generation = consumed_generation;
	            lock.unlock();
	            auto wait_start = std::chrono::steady_clock::now();
	            trigger_pipewire_process();
	            lock.lock();
	            frame_cv.wait_for(lock, frame_interval, [&]() {
	              return !running || request_mapped_reinit_.load() || latest_generation != wanted_generation;
	            });
	            wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();

	            if (!running) {
	              return capture_e::interrupted;
	            }

	            if (request_mapped_reinit_.load()) {
	              return capture_e::reinit;
	            }

	            if (latest_dmabuf.valid && latest_generation != consumed_generation) {
	              auto descriptor = static_cast<egl::img_descriptor_t *>(img.get());
	              descriptor->width = width;
	              descriptor->height = height;
	              descriptor->pixel_pitch = 4;
	              descriptor->row_pitch = width * 4;
	              descriptor->sequence = latest_generation;
	              descriptor->serial = std::numeric_limits<decltype(descriptor->serial)>::max();
	              // This image takes ownership of the buffer; it is re-queued only when
	              // the image is pulled again, i.e. after the encoder has imported it.
	              auto *consumed_buf = latest_dmabuf.pw_buf;
	              latest_dmabuf.pw_buf = nullptr;
	              latest_dmabuf.move_to(*descriptor);
	              dmabuf_img->pw_buf = consumed_buf;
	              consumed_generation = latest_generation;
	              captured = true;
	            }
	          }

	          log_capture_diag(captured, wait_ms, 0);

	          if (!push_captured_image_cb(std::move(img), captured)) {
	            return capture_e::ok;
	          }
	        }

	        return capture_e::interrupted;
	      }

	      std::shared_ptr<img_t> alloc_img() override {
	        if (active_capture_mode == pipewire_capture_mode_e::DMABUF) {
	          auto img = std::make_shared<pw_dmabuf_img_t>();
	          img->width = width;
	          img->height = height;
	          img->pixel_pitch = 4;
	          img->row_pitch = width * 4;
	          img->sequence = 0;
	          img->serial = std::numeric_limits<decltype(img->serial)>::max();
	          img->data = nullptr;
	          std::fill_n(img->sd.fds, 4, -1);
	          return img;
	        }

	        auto img = std::make_shared<pipewire_img_t>();
	        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = width * 4;
        img->storage.assign(static_cast<std::size_t>(img->row_pitch) * height, 0);
        img->data = img->storage.data();
        return img;
	      }

	      int dummy_img(img_t *img) override {
	        if (active_capture_mode == pipewire_capture_mode_e::DMABUF) {
	          auto descriptor = static_cast<egl::img_descriptor_t *>(img);
	          descriptor->reset();
	          descriptor->sequence = 0;
	          descriptor->data = nullptr;
	          descriptor->width = width;
	          descriptor->height = height;
	          descriptor->pixel_pitch = 4;
	          descriptor->row_pitch = width * 4;
	          return 0;
	        }

	        if (!img || !img->data) {
	          return -1;
	        }

        std::memset(img->data, 0, static_cast<std::size_t>(img->row_pitch) * img->height);
        img->frame_timestamp = std::chrono::steady_clock::now();
        return 0;
      }

	      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e) override {
#ifdef SUNSHINE_BUILD_VAAPI
	        if (mem_type == mem_type_e::vaapi) {
	          if (active_capture_mode == pipewire_capture_mode_e::DMABUF) {
	            return va::make_avcodec_encode_device(width, height, 0, 0, true);
	          }
	          return va::make_avcodec_encode_device(width, height, false);
	        }
#endif

#ifdef SUNSHINE_BUILD_CUDA
	        if (mem_type == mem_type_e::cuda) {
	          if (active_capture_mode == pipewire_capture_mode_e::DMABUF) {
	            if (encode_drm_fd_ >= 0) {
	              return cuda::make_avcodec_gl_encode_device(width, height, 0, 0, encode_drm_fd_);
	            }
	            return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
	          }
	          return cuda::make_avcodec_encode_device(width, height, false);
	        }
#endif

        return std::make_unique<avcodec_encode_device_t>();
      }

	    private:
	      void configure_dmabuf_policy() {
	        const char *environment_override = std::getenv("APOLLO_PIPEWIRE_DMABUF");
	        if (environment_override && *environment_override && !VDISPLAY::parseLinuxPipeWireDmaBuf(environment_override)) {
	          BOOST_LOG(warning) << "Unknown APOLLO_PIPEWIRE_DMABUF="sv << environment_override << "; ignoring environment override.";
	        }
	        if (!config::video.linux_pipewire_dmabuf.empty() && !VDISPLAY::parseLinuxPipeWireDmaBuf(config::video.linux_pipewire_dmabuf)) {
	          BOOST_LOG(warning) << "Unknown linux_pipewire_dmabuf="sv << config::video.linux_pipewire_dmabuf << "; defaulting to off.";
	        }

	        dmabuf_policy = VDISPLAY::resolveLinuxPipeWireDmaBuf(config::video.linux_pipewire_dmabuf, environment_override);

	        // A previous AUTO DMA-BUF import already failed this process: stay on the
	        // mapped path for every subsequent (re)init so we never loop.
	        if (g_pipewire_dmabuf_disabled.load() && dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::AUTO) {
	          dmabuf_allowed = false;
	          BOOST_LOG(info) << "GNOME PipeWire DMA-BUF previously fell back to mapped this session; using mapped capture.";
	          return;
	        }

	        const bool encoder_can_import_dmabuf =
#ifdef SUNSHINE_BUILD_CUDA
	          mem_type == mem_type_e::cuda ||
#endif
#ifdef SUNSHINE_BUILD_VAAPI
	          mem_type == mem_type_e::vaapi ||
#endif
	          false;
		        const char *gamescope_dmabuf_override = std::getenv("APOLLO_GAMESCOPE_DMABUF");
		        const bool gamescope_dmabuf_disabled =
		          gamescope_dmabuf_override &&
		          (std::strcmp(gamescope_dmabuf_override, "0") == 0 ||
		           std::strcmp(gamescope_dmabuf_override, "off") == 0 ||
		           std::strcmp(gamescope_dmabuf_override, "false") == 0);
		        // AUTO now attempts DMA-BUF just like FORCE does for the OFFER, but with a
		        // graceful runtime fallback to mapped (see on_stream_process). FORCE keeps
		        // its hard-fail-on-miss semantics.
		        dmabuf_allowed = (dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::FORCE ||
		                          dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::AUTO ||
		                          (gamescope_pipewire_capture && !gamescope_dmabuf_disabled)) &&
		                         encoder_can_import_dmabuf;
		        if (dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::FORCE && !encoder_can_import_dmabuf) {
		          dmabuf_policy_error = true;
		          BOOST_LOG(error) << "PipeWire DMA-BUF capture was forced, but the selected encoder path cannot import DMA-BUF frames.";
		        } else if (dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::AUTO && encoder_can_import_dmabuf) {
		          BOOST_LOG(info) << "PipeWire DMA-BUF auto negotiation is enabled; will fall back to mapped capture if import fails.";
		        } else if (gamescope_pipewire_capture && gamescope_dmabuf_disabled) {
		          BOOST_LOG(info) << "Gamescope/PipeWire DMA-BUF offer is disabled by APOLLO_GAMESCOPE_DMABUF.";
		        } else if (gamescope_pipewire_capture && encoder_can_import_dmabuf) {
		          BOOST_LOG(info) << "Gamescope/PipeWire capture will offer DMA-BUF first to match Gamescope's native stream path.";
		        }

		        // Resolve and open the render node for the encode/import GPU once, while
		        // DMA-BUF is eligible. Everything (encode device, import display, CUDA
		        // device index) follows this fd; -1 keeps the legacy CUDA-device-0 path.
		        if (dmabuf_allowed && encode_drm_fd_ < 0) {
		          bool manual_override = false;
		          const std::string render_node = resolve_dmabuf_encode_render_node(manual_override);
		          if (!render_node.empty()) {
		            encode_drm_fd_ = open(render_node.c_str(), O_RDWR | O_CLOEXEC);
		            if (encode_drm_fd_ < 0) {
		              BOOST_LOG(warning) << "DMA-BUF encode GPU: failed to open " << render_node
		                                 << " (" << strerror(errno) << "); using default CUDA device.";
		            } else {
		              BOOST_LOG(info) << "DMA-BUF encode GPU: " << render_node
		                              << " (" << (manual_override ? "adapter_name" : "auto") << ')';
		            }
		          } else {
		            BOOST_LOG(info) << "DMA-BUF encode GPU: none auto-detected; using default CUDA device.";
		          }
		        }

	        BOOST_LOG(info) << "GNOME PipeWire DMA-BUF policy is "sv << VDISPLAY::linuxPipeWireDmaBufName(dmabuf_policy)
	                        << "; capture import is " << (dmabuf_allowed ? "eligible" : "mapped");
	      }

	      // Query the importer's accepted DRM modifier set ONCE, on the SAME DRM node the
	      // encoder opens, so the modifiers advertised in EnumFormat intersect what Mutter
	      // composites into (NVIDIA block-linear). On failure the list stays empty and the
	      // build_*_format functions fall back to the LINEAR-only offer (no regression).
	      void query_importable_modifiers() {
	        importable_modifiers_.clear();

#ifdef SUNSHINE_BUILD_CUDA
	        if (mem_type == mem_type_e::cuda) {
	          // BGRx -> XRGB8888, BGRA -> ARGB8888 (the fourccs Apollo imports).
	          // Probe on the resolved encode GPU (encode_drm_fd_), or CUDA device 0 (-1).
	          importable_modifiers_ = cuda::query_importable_modifiers({DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888}, encode_drm_fd_);
	        }
#endif

	        if (importable_modifiers_.empty()) {
	          BOOST_LOG(warning) << "GNOME PipeWire DMA-BUF: no importable modifiers were discovered; "
	                                "falling back to the LINEAR-only modifier offer.";
	        } else {
	          BOOST_LOG(info) << "GNOME PipeWire DMA-BUF: discovered " << importable_modifiers_.size()
	                          << " importable modifier(s); advertising them in EnumFormat.";
	        }
	      }

      void update_buffer_data_type(bool use_dmabuf) {
        std::uint8_t params_buffer[512];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
        const auto data_types = use_dmabuf ? (1 << SPA_DATA_DmaBuf) :
                                gamescope_pipewire_capture ? (1 << SPA_DATA_MemFd) :
                                ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
        const auto frame_width = latest_width > 0 ? latest_width : width;
        const auto frame_height = latest_height > 0 ? latest_height : height;
        const auto stride = frame_width * 4;
        const auto size = stride * frame_height;

        std::array<const spa_pod *, 2> params {};
        std::uint32_t n_params = 0;

        // Minimal 2-buffer pool (PipeWire's floor). A larger pool lets Mutter's ScreenCast
        // re-deliver a longer run of stale buffers when rendering is idle; 2 bounds any residual
        // stale window to a single frame. Empty re-deliveries are dropped in on_stream_process
        // (chunk->size == 0), which is the primary fix; this small pool is the backstop.
        params[n_params++] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_ParamBuffers,
            SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 2, 2),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_types)
          ));
        params[n_params++] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
          &builder,
          SPA_TYPE_OBJECT_ParamMeta,
          SPA_PARAM_Meta,
          SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
          SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header))
        ));
        if (pw_stream_update_params(stream, params.data(), n_params) < 0) {
          BOOST_LOG(warning) << "Unable to update PipeWire buffer data type to "
                             << (use_dmabuf ? "DMA-BUF" : "mapped memory") << '.';
        }
	      }

	      static void on_stream_state_changed(void *data, pw_stream_state old_state, pw_stream_state state, const char *stream_error) {
	        auto self = static_cast<pipewire_display_t *>(data);
	        BOOST_LOG(info) << "PipeWire capture stream state changed for " << self->display_name
	                        << ": " << pw_stream_state_as_string(old_state)
	                        << " -> " << pw_stream_state_as_string(state)
	                        << (stream_error ? stream_error : "");
        if (state == PW_STREAM_STATE_ERROR) {
          BOOST_LOG(error) << "GNOME PipeWire stream error: "sv << (stream_error ? stream_error : "unknown");
          self->running = false;
          self->frame_cv.notify_all();
        }
      }

      static void on_stream_param_changed(void *data, uint32_t id, const spa_pod *param) {
        auto self = static_cast<pipewire_display_t *>(data);
        if (!param || id != SPA_PARAM_Format) {
          return;
        }

        // Parse media type/subtype: only raw video formats are negotiated here.
        std::uint32_t media_type = 0;
        std::uint32_t media_subtype = 0;
        if (spa_format_parse(param, &media_type, &media_subtype) < 0) {
          return;
        }
        if (media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw) {
          return;
        }

        spa_video_info_raw video_info {};
        if (spa_format_video_raw_parse(param, &video_info) < 0) {
          return;
        }

        // Inspect the modifier property directly: on the FIRST param_changed of a
        // DONT_FIXATE handshake it is still an UNFIXED CHOICE and the parsed
        // video_info.modifier is meaningless (yields 0/LINEAR which we never offered).
        const spa_pod_prop *modifier_prop = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
        const bool modifier_is_unfixed_choice =
          modifier_prop != nullptr &&
          spa_pod_is_choice(&modifier_prop->value) &&
          SPA_POD_CHOICE_TYPE(&modifier_prop->value) != SPA_CHOICE_None;

        // CASE 1 — unfixed modifier choice: pick the first offered value that we can
        // import and re-announce a single, fixed EnumFormat (no DONT_FIXATE, no choice).
        // The server will then re-send param_changed with the now-fixed format (CASE 2).
        if (modifier_is_unfixed_choice && self->dmabuf_allowed && !self->dmabuf_modifier_fixated) {
          std::uint64_t chosen_modifier = DRM_FORMAT_MOD_INVALID;
          bool found = false;

          const std::uint32_t n_values = SPA_POD_CHOICE_N_VALUES(&modifier_prop->value);
          const std::uint64_t *values =
            static_cast<const std::uint64_t *>(SPA_POD_CHOICE_VALUES(&modifier_prop->value));
          for (std::uint32_t i = 0; i < n_values; ++i) {
            const std::uint64_t candidate = values[i];
            if (std::find(self->importable_modifiers_.begin(), self->importable_modifiers_.end(), candidate) !=
                self->importable_modifiers_.end()) {
              chosen_modifier = candidate;
              found = true;
              break;
            }
          }

          if (found) {
            std::uint8_t fixate_buffer[1024];
            spa_pod_builder builder = SPA_POD_BUILDER_INIT(fixate_buffer, sizeof(fixate_buffer));
            const spa_rectangle fixed_size {video_info.size.width, video_info.size.height};
            const spa_fraction max_rate {self->framerate, 1};
            const spa_fraction min_rate {1, 1};

            spa_pod_frame format_frame {};
            spa_pod_builder_push_object(&builder, &format_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(
              &builder,
              SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
              SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
              SPA_FORMAT_VIDEO_format, SPA_POD_Id(video_info.format),
              SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&fixed_size),
              SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&max_rate, &min_rate, &max_rate),
              0
            );
            spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_long(&builder, static_cast<std::int64_t>(chosen_modifier));
            const spa_pod *fixate_format = static_cast<const spa_pod *>(spa_pod_builder_pop(&builder, &format_frame));

            const spa_pod *params[1] = {fixate_format};
            self->dmabuf_modifier_fixated = true;
            BOOST_LOG(info) << "GNOME PipeWire DMA-BUF: fixating DMA-BUF modifier 0x"
                            << std::hex << chosen_modifier << std::dec;
            if (pw_stream_update_params(self->stream, params, 1) < 0) {
              BOOST_LOG(warning) << "GNOME PipeWire DMA-BUF: failed to update params for modifier fixation.";
            }
            // Wait for the server to re-send param_changed with the fixed format.
            return;
          }
          // No offered choice value is importable: fall through and treat as final/mapped.
        }

        // CASE 2 — final format: a single fixed modifier value, a mapped format with no
        // modifier, or an unfixed choice with no importable match. Commit it and update
        // buffers/meta only. Do NOT re-emit any EnumFormat here.
        const bool negotiated_dmabuf =
          (video_info.flags & SPA_VIDEO_FLAG_MODIFIER) != 0 && !modifier_is_unfixed_choice;
        const bool use_dmabuf = self->dmabuf_allowed && negotiated_dmabuf;

        {
          std::lock_guard lock(self->frame_mutex);
          self->pipewire_format = video_info.format;
          self->pipewire_modifier = negotiated_dmabuf ? video_info.modifier : DRM_FORMAT_MOD_INVALID;
          self->active_capture_mode = use_dmabuf ? pipewire_capture_mode_e::DMABUF : pipewire_capture_mode_e::MAPPED;
          if (video_info.size.width > 0 && video_info.size.height > 0) {
            self->latest_width = static_cast<int>(video_info.size.width);
            self->latest_height = static_cast<int>(video_info.size.height);
            self->width = self->latest_width;
            self->height = self->latest_height;
            self->env_width = self->width;
            self->env_height = self->height;
          }

          if (self->dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::FORCE && !use_dmabuf) {
            self->format_failed = true;
          }
          self->format_ready = true;
        }

        self->update_buffer_data_type(use_dmabuf);
        self->format_cv.notify_all();

        BOOST_LOG(info) << "GNOME PipeWire capture format "
                        << self->latest_width << 'x' << self->latest_height
                        << " spa_format=" << static_cast<int>(self->pipewire_format)
                        << " modifier=" << self->pipewire_modifier
                        << " capture_path=" << pipewire_capture_mode_name(self->active_capture_mode);

        // RecordVirtual desktop layout: once a format is negotiated the Mutter virtual head
        // (Meta-0) has materialized, so make it the sole primary to relocate the real desktop
        // onto it. One-shot (atomic), off-thread (helper runs a subprocess up to ~8s).
        if (self->is_mutter_pipewire) {
          bool expected = false;
          if (self->mutter_layout_applied.compare_exchange_strong(expected, true)) {
            BOOST_LOG(info) << "GNOME PipeWire format ready for " << self->display_name
                            << "; applying RecordVirtual desktop layout (isolate).";
            std::string layout_name = self->display_name;
            std::thread([layout_name]() {
              VDISPLAY::applyMutterDisplayLayout(layout_name, true);
            }).detach();
          }
        }
      }

	      static void on_stream_process(void *data) {
		        auto self = static_cast<pipewire_display_t *>(data);
		        auto buffer = pw_stream_dequeue_buffer(self->stream);
	        if (!buffer) {
	          return;
	        }

	        auto spa_buffer = buffer->buffer;
	        if (!spa_buffer || spa_buffer->n_datas == 0 || !spa_buffer->datas[0].chunk) {
	          pw_stream_queue_buffer(self->stream, buffer);
	          return;
	        }

	        const auto &data0 = spa_buffer->datas[0];
	        const auto *chunk = data0.chunk;
	        // Mutter re-delivers stale pool buffers with chunk->size == 0 (no new pixel data) when
	        // nothing was rendered this frame -- tapping Ctrl/Shift, typing pauses, idle, cursor
	        // stops. The buffer still holds an OLD frame, so encoding it shows past content (the
	        // "revert to old frames / doesn't paint my keystrokes" glitch). Drop it and hold the
	        // last real frame. Applies to both mapped and DMA-BUF capture.
	        if (chunk && chunk->size == 0) {
	          pw_stream_queue_buffer(self->stream, buffer);
	          return;
	        }
	        if (data0.type == SPA_DATA_DmaBuf && self->active_capture_mode == pipewire_capture_mode_e::DMABUF) {
	          const auto import_start = std::chrono::steady_clock::now();
	          auto frame = self->make_dmabuf_frame(spa_buffer);
	          if (!frame) {
	            if (self->dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::FORCE) {
	              BOOST_LOG(error) << "Forced PipeWire DMA-BUF capture received an unimportable buffer; stopping capture.";
	              self->running = false;
	              self->frame_cv.notify_all();
	            } else if (self->dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::AUTO) {
	              // AUTO: never break video, fall back to mapped (rebuilds the session once).
	              self->fall_back_to_mapped_capture();
	            }
	            pw_stream_queue_buffer(self->stream, buffer);
	            return;
	          }

	          frame->pw_buf = buffer;
	          {
	            std::lock_guard lock(self->frame_mutex);
	            // A previously produced frame was never consumed by the capture thread
	            // (it is being dropped). Recycle its buffer here on the PipeWire loop
	            // thread before we overwrite the slot, otherwise it would leak.
	            if (self->latest_dmabuf.valid && self->latest_dmabuf.pw_buf) {
	              pw_stream_queue_buffer(self->stream, self->latest_dmabuf.pw_buf);
	              self->latest_dmabuf.pw_buf = nullptr;
	            }
	            self->latest_dmabuf = std::move(*frame);
	            self->latest_timestamp = self->latest_dmabuf.timestamp;
	            ++self->latest_generation;
	          }
	          self->frame_cv.notify_all();
	          self->log_process_diag(
	            0,
	            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - import_start).count(),
	            data0.type,
	            chunk->stride
	          );

	          // NOTE: do NOT re-queue the buffer here. It stays checked out of the
	          // PipeWire pool until the encoder has finished importing this frame, so
	          // the compositor cannot render new content into a buffer we still read
	          // from. capture_dmabuf re-queues it once the image returns to the pool.
	          return;
	        }

	        if (data0.type == SPA_DATA_DmaBuf && !data0.data) {
	          if (!self->logged_unexpected_unmapped_dmabuf) {
	            BOOST_LOG(warning) << "PipeWire delivered an unmapped DMA-BUF after Apollo selected mapped capture; dropping frames until PipeWire renegotiates.";
	            self->logged_unexpected_unmapped_dmabuf = true;
	          }
	          pw_stream_queue_buffer(self->stream, buffer);
	          return;
	        }

	        if (!data0.data) {
	          if (self->dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::FORCE) {
	            BOOST_LOG(error) << "Forced PipeWire DMA-BUF capture received mapped buffer data_type=" << data0.type << "; stopping capture.";
	            self->running = false;
	            self->frame_cv.notify_all();
	          }
	          pw_stream_queue_buffer(self->stream, buffer);
	          return;
	        }

	        const auto src_stride = chunk->stride > 0 ? chunk->stride : static_cast<int32_t>(self->width * 4);
	        const auto src = static_cast<const std::uint8_t *>(data0.data) + chunk->offset;
	        const auto frame_width = self->latest_width > 0 ? self->latest_width : self->width;
	        const auto frame_height = self->latest_height > 0 ? self->latest_height : self->height;
	        const auto row_bytes = static_cast<std::size_t>(std::min<int>(std::abs(src_stride), frame_width * 4));
	        const auto copy_start = std::chrono::steady_clock::now();

	        {
	          std::lock_guard lock(self->frame_mutex);
	          self->latest_stride = frame_width * 4;
	          self->latest_pixels.resize(static_cast<std::size_t>(self->latest_stride) * frame_height);
	          for (int y = 0; y < frame_height; ++y) {
	            auto src_row = src + (static_cast<std::size_t>(y) * std::abs(src_stride));
	            auto dst_row = self->latest_pixels.data() + (static_cast<std::size_t>(y) * self->latest_stride);
	            std::memcpy(dst_row, src_row, row_bytes);
	          }
          self->latest_timestamp = std::chrono::steady_clock::now();
	          ++self->latest_generation;
	        }
	        self->frame_cv.notify_all();
	        self->log_process_diag(
	          static_cast<std::size_t>(row_bytes) * frame_height,
	          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copy_start).count(),
	          data0.type,
	          src_stride
	        );

		        pw_stream_queue_buffer(self->stream, buffer);
		      }

	      std::optional<dmabuf_frame_t> make_dmabuf_frame(spa_buffer *spa_buffer) {
	        auto fourcc = pipewire_format_to_drm_fourcc(pipewire_format);
	        if (!fourcc) {
	          BOOST_LOG(warning) << "PipeWire DMA-BUF format is not importable by Apollo: spa_format="
	                             << static_cast<int>(pipewire_format);
	          return std::nullopt;
	        }

	        dmabuf_frame_t frame;
	        frame.sd.width = latest_width > 0 ? latest_width : width;
	        frame.sd.height = latest_height > 0 ? latest_height : height;
	        frame.sd.fourcc = *fourcc;
	        frame.sd.modifier = pipewire_modifier;

	        const auto plane_count = std::min<std::uint32_t>(spa_buffer->n_datas, 4);
	        for (std::uint32_t plane = 0; plane < plane_count; ++plane) {
	          const auto &data = spa_buffer->datas[plane];
	          if (data.type != SPA_DATA_DmaBuf || data.fd < 0 || !data.chunk) {
	            continue;
	          }

	          auto fd = dup(static_cast<int>(data.fd));
	          if (fd < 0) {
	            BOOST_LOG(warning) << "Unable to duplicate PipeWire DMA-BUF fd for plane " << plane << ": " << strerror(errno);
	            return std::nullopt;
	          }

	          frame.sd.fds[plane] = fd;
	          frame.sd.pitches[plane] = data.chunk->stride > 0 ? static_cast<std::uint32_t>(data.chunk->stride) : static_cast<std::uint32_t>(width * 4);
	          frame.sd.offsets[plane] = data.chunk->offset;
	        }

	        if (frame.sd.fds[0] < 0) {
	          BOOST_LOG(warning) << "PipeWire DMA-BUF buffer did not include an importable first plane.";
	          return std::nullopt;
	        }

	        frame.timestamp = std::chrono::steady_clock::now();
	        frame.valid = true;
	        return frame;
	      }

	      bool start_mutter_stream() {
        if (VDISPLAY::virtualDisplayBackend(display_name) == VDISPLAY::BACKEND::MUTTER_PIPEWIRE) {
          std::uint32_t backend_node_id {};
          if (!VDISPLAY::getMutterPipeWireNodeId(display_name, backend_node_id)) {
            BOOST_LOG(error) << "Mutter/PipeWire virtual display ["sv << display_name
                             << "] has no PipeWire node; refusing capture fallback."sv;
            return false;
          }

          node_id = backend_node_id;
          owns_mutter_session = false;
          BOOST_LOG(info) << "Using backend-owned Mutter/PipeWire node " << node_id
                          << " for virtual display [" << display_name << ']';
          return true;
        }

        if (VDISPLAY::virtualDisplayBackend(display_name) == VDISPLAY::BACKEND::GAMESCOPE_PIPEWIRE) {
          std::uint32_t backend_node_id {};
          if (!VDISPLAY::getGamescopePipeWireNodeId(display_name, backend_node_id)) {
            BOOST_LOG(error) << "Gamescope/PipeWire virtual display ["sv << display_name
                             << "] has no PipeWire node; refusing capture fallback."sv;
            return false;
          }

          node_id = backend_node_id;
          owns_mutter_session = false;
          BOOST_LOG(info) << "Using backend-owned Gamescope/PipeWire node " << node_id
                          << " for virtual display [" << display_name << ']';
          return true;
        }

	        GError *raw_error = nullptr;
	        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &raw_error);
        if (!bus) {
          mutter_dbus::gerror_ptr dbus_error(raw_error);
          BOOST_LOG(error) << "Unable to connect to session bus for GNOME PipeWire capture: "sv << (dbus_error ? dbus_error->message : "unknown");
          return false;
        }

        GVariantBuilder session_props;
        g_variant_builder_init(&session_props, G_VARIANT_TYPE("a{sv}"));

        auto session_result = mutter_dbus::call_sync(
          bus,
          mutter_dbus::SCREENCAST_SERVICE,
          mutter_dbus::SCREENCAST_PATH,
          "org.gnome.Mutter.ScreenCast",
          "CreateSession",
          g_variant_new("(a{sv})", &session_props),
          G_VARIANT_TYPE("(o)"),
          mutter_dbus::STREAM_CALL_TIMEOUT_MS,
          &raw_error
        );
        if (!session_result) {
          mutter_dbus::gerror_ptr dbus_error(raw_error);
          BOOST_LOG(error) << "Unable to create GNOME ScreenCast session: "sv << (dbus_error ? dbus_error->message : "unknown");
          return false;
        }

        const char *session_path_raw = nullptr;
        g_variant_get(session_result, "(&o)", &session_path_raw);
        session_path = session_path_raw;
        g_variant_unref(session_result);

        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&props, "{sv}", "cursor-mode", g_variant_new_uint32(1));
        g_variant_builder_add(&props, "{sv}", "is-recording", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&props, "{sv}", "is-platform", g_variant_new_boolean(TRUE));
	        g_variant_builder_add(&props, "{sv}", "width", g_variant_new_uint32(width));
	        g_variant_builder_add(&props, "{sv}", "height", g_variant_new_uint32(height));
	        g_variant_builder_add(&props, "{sv}", "framerate", g_variant_new_uint32(framerate));

	        raw_error = nullptr;
	        auto stream_result = mutter_dbus::call_sync(
	          bus,
	          mutter_dbus::SCREENCAST_SERVICE,
	          session_path.c_str(),
	          "org.gnome.Mutter.ScreenCast.Session",
	          "RecordVirtual",
	          g_variant_new("(a{sv})", &props),
	          G_VARIANT_TYPE("(o)"),
	          mutter_dbus::STREAM_CALL_TIMEOUT_MS,
          &raw_error
        );
	        if (!stream_result) {
	          mutter_dbus::gerror_ptr dbus_error(raw_error);
	          BOOST_LOG(error) << "Unable to create GNOME virtual ScreenCast stream: "sv
	                           << (dbus_error ? dbus_error->message : "unknown");
	          return false;
	        }

        const char *stream_path_raw = nullptr;
        g_variant_get(stream_result, "(&o)", &stream_path_raw);
        stream_path = stream_path_raw;
        g_variant_unref(stream_result);

        signal_id = g_dbus_connection_signal_subscribe(
          bus,
          mutter_dbus::SCREENCAST_SERVICE,
          "org.gnome.Mutter.ScreenCast.Stream",
          "PipeWireStreamAdded",
          stream_path.c_str(),
          nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE,
          [](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *parameters, gpointer user_data) {
            auto self = static_cast<pipewire_display_t *>(user_data);
            guint node = 0;
            g_variant_get(parameters, "(u)", &node);
            {
              std::lock_guard lock(self->node_mutex);
              self->node_id = node;
            }
            self->node_cv.notify_all();
          },
          this,
          nullptr
        );

        if (!call_no_args(session_path, "org.gnome.Mutter.ScreenCast.Session", "Start")) {
          return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
          while (g_main_context_iteration(nullptr, FALSE)) {
          }
          {
            std::lock_guard lock(node_mutex);
            if (node_id) {
              break;
            }
          }
          std::this_thread::sleep_for(20ms);
        }

        if (!node_id) {
          BOOST_LOG(error) << "Timed out waiting for GNOME PipeWire stream node.";
          return false;
        }

        BOOST_LOG(info) << "GNOME ScreenCast PipeWire node " << node_id << " created.";
	        return true;
	      }

	      bool call_no_args(const std::string &path, const char *interface, const char *method) {
        return mutter_dbus::call_no_args(
          bus,
          mutter_dbus::SCREENCAST_SERVICE,
          path.c_str(),
          interface,
          method,
          mutter_dbus::STREAM_CALL_TIMEOUT_MS,
          "GNOME ScreenCast"
        );
      }

	      // Build the EnumFormat params for the current dmabuf_allowed state and connect
	      // the stream. The PipeWire thread loop MUST be locked by the caller. Shared by
	      // the initial connect and the AUTO mapped-fallback reconnect path.
	      bool connect_stream_params_locked() {
	        if (dmabuf_allowed) {
	          query_importable_modifiers();
	        }

	        std::uint8_t params_buffer[4096];
	        spa_pod_builder builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	        const spa_rectangle requested_size {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
	        const spa_rectangle min_size {1, 1};
	        const spa_fraction no_fixed_rate {0, 1};
	        const spa_fraction requested_rate {framerate, 1};
	        const spa_fraction min_rate {1, 1};

	        std::array<const spa_pod *, 6> params {};
	        std::uint32_t n_params = 0;

	        if (gamescope_pipewire_capture) {
	          if (dmabuf_allowed) {
	            params[n_params++] = build_gamescope_pipewire_format(&builder, true, requested_size, min_size, requested_rate);
	          }
	          params[n_params++] = build_gamescope_pipewire_format(&builder, false, requested_size, min_size, requested_rate);
	        } else {
	          if (dmabuf_allowed) {
	            params[n_params++] = build_pipewire_format(&builder, SPA_VIDEO_FORMAT_BGRx, true, requested_size, min_size, no_fixed_rate, requested_rate, min_rate);
	            params[n_params++] = build_pipewire_format(&builder, SPA_VIDEO_FORMAT_BGRA, true, requested_size, min_size, no_fixed_rate, requested_rate, min_rate);
	          }

	          if (dmabuf_policy != VDISPLAY::PIPEWIRE_DMABUF::FORCE) {
	            params[n_params++] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
	              &builder,
	              SPA_TYPE_OBJECT_Format,
	              SPA_PARAM_EnumFormat,
	              SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
	              SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	              SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
	              SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&requested_size, &min_size, &requested_size),
	              SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&no_fixed_rate),
	              SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&requested_rate, &min_rate, &requested_rate)
	            ));
	          }
	        }

	        if (n_params == 0) {
	          BOOST_LOG(error) << "No PipeWire capture formats are available for the selected DMA-BUF policy.";
	          return false;
	        }

	        const auto target_id = gamescope_pipewire_capture ? PW_ID_ANY : node_id;
	        if (gamescope_pipewire_capture) {
	          BOOST_LOG(info) << "Connecting Gamescope PipeWire capture via target.object=" << GAMESCOPE_PIPEWIRE_TARGET
	                          << " node_hint=" << node_id;
	        }
	        // Reset the two-step DMA-BUF handshake guard so a (re)connect re-runs fixation.
	        dmabuf_modifier_fixated = false;
	        if (pw_stream_connect(stream, PW_DIRECTION_INPUT, target_id, stream_flags, params.data(), n_params) < 0) {
	          BOOST_LOG(error) << "Unable to connect PipeWire stream to "
	                           << (gamescope_pipewire_capture ? GAMESCOPE_PIPEWIRE_TARGET : "GNOME node")
	                           << ' ' << node_id;
	          return false;
	        }
	        return true;
	      }

	      // AUTO graceful fallback: a DMA-BUF buffer could not be imported. Disable
	      // DMA-BUF for the rest of the process (sticky, anti-loop) and ask the video
	      // core to rebuild the session in mapped mode. The sticky decision is process
	      // -wide (g_pipewire_dmabuf_disabled) so that the capture_e::reinit teardown,
	      // which destroys and recreates this display + its encode device, comes back on
	      // the mapped path instead of re-offering DMA-BUF and looping. We use reinit
	      // (not an in-place stream reconnect) because the encode device itself differs
	      // between the two paths: the DMA-BUF path builds a GL->CUDA importer while the
	      // mapped path builds the RAM-upload encoder, so both must be rebuilt together.
	      void fall_back_to_mapped_capture() {
	        bool expected = false;
	        if (!dmabuf_disabled_for_session_.compare_exchange_strong(expected, true)) {
	          return;  // already falling back / fell back: never loop
	        }
	        g_pipewire_dmabuf_disabled.store(true);
	        request_mapped_reinit_.store(true);
	        BOOST_LOG(warning) << "DMA-BUF import failed; falling back to mapped PipeWire capture.";
	        // Wake the capture loop so it observes the reinit request promptly.
	        frame_cv.notify_all();
	      }

	      bool start_pipewire_stream() {
	        if (dmabuf_policy_error) {
	          return false;
	        }

	        pw_init(nullptr, nullptr);

        loop = pw_thread_loop_new("apollo-pipewire-capture", nullptr);
        if (!loop) {
          BOOST_LOG(error) << "Unable to create PipeWire thread loop."sv;
          return false;
        }

        pw_thread_loop_lock(loop);

        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        if (!context) {
          pw_thread_loop_unlock(loop);
          BOOST_LOG(error) << "Unable to create PipeWire context."sv;
          return false;
        }

        core = pw_context_connect(context, nullptr, 0);
        if (!core) {
          pw_thread_loop_unlock(loop);
          BOOST_LOG(error) << "Unable to connect PipeWire context."sv;
          return false;
        }

        static pw_stream_events events {};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = on_stream_state_changed;
        events.param_changed = on_stream_param_changed;
        events.process = on_stream_process;

        const auto stream_role = gamescope_pipewire_capture ? "Camera" : "Screen";
        auto stream_properties = pw_properties_new(
          PW_KEY_MEDIA_TYPE, "Video",
          PW_KEY_MEDIA_CATEGORY, "Capture",
          PW_KEY_MEDIA_ROLE, stream_role,
          nullptr
        );
        if (gamescope_pipewire_capture) {
          pw_properties_set(stream_properties, PW_KEY_TARGET_OBJECT, GAMESCOPE_PIPEWIRE_TARGET);
        }

        stream = pw_stream_new_simple(
	          pw_thread_loop_get_loop(loop),
	          gamescope_pipewire_capture ? "apollo-gamescope-capture" : "apollo-gnome-screencast",
	          stream_properties,
	          &events,
	          this
	        );
        if (!stream) {
          pw_thread_loop_unlock(loop);
          BOOST_LOG(error) << "Unable to create PipeWire stream."sv;
          return false;
        }

        auto flags = static_cast<pw_stream_flags>(
          PW_STREAM_FLAG_AUTOCONNECT |
          PW_STREAM_FLAG_MAP_BUFFERS
        );
        if (gamescope_pipewire_capture) {
          flags = static_cast<pw_stream_flags>(flags | PW_STREAM_FLAG_DRIVER);
        } else {
          flags = static_cast<pw_stream_flags>(flags | PW_STREAM_FLAG_RT_PROCESS);
        }

	        stream_flags = flags;
	        if (!connect_stream_params_locked()) {
	          pw_thread_loop_unlock(loop);
	          return false;
	        }

        if (pw_thread_loop_start(loop) < 0) {
          pw_thread_loop_unlock(loop);
          BOOST_LOG(error) << "Unable to start PipeWire thread loop."sv;
          return false;
        }

	        if (pw_stream_set_active(stream, true) < 0) {
	          pw_thread_loop_unlock(loop);
	          BOOST_LOG(error) << "Unable to activate PipeWire capture stream for node " << node_id;
	          return false;
	        }

	        if (gamescope_pipewire_capture) {
	          gamescope_drives_pipewire = pw_stream_is_driving(stream);
	          BOOST_LOG(info) << "Gamescope PipeWire consumer graph driver is "
	                          << (gamescope_drives_pipewire ? "active" : "not active");
	        }

	        pw_thread_loop_unlock(loop);

	        running = true;

	        if (dmabuf_allowed) {
	          std::unique_lock lock(frame_mutex);
	          if (!format_cv.wait_for(lock, 2s, [&]() {
	                return format_ready || format_failed || !running;
	              })) {
	            if (dmabuf_policy == VDISPLAY::PIPEWIRE_DMABUF::FORCE) {
	              running = false;
	              BOOST_LOG(error) << "Timed out waiting for forced PipeWire DMA-BUF format negotiation.";
	              return false;
	            }
	            dmabuf_allowed = false;
	            active_capture_mode = pipewire_capture_mode_e::MAPPED;
	            BOOST_LOG(warning) << "Timed out waiting for PipeWire DMA-BUF negotiation; using mapped PipeWire capture.";
	          } else if (format_failed) {
	            running = false;
	            BOOST_LOG(error) << "PipeWire DMA-BUF capture was forced, but Mutter did not negotiate DMA-BUF.";
	            return false;
	          }
	        }

	        process_diag_at = std::chrono::steady_clock::now();
	        BOOST_LOG(info) << "GNOME PipeWire capture path selected: "sv << pipewire_capture_mode_name(active_capture_mode);
	        return true;
		      }

	      // Return a PipeWire buffer to the pool from the capture (consumer) thread.
	      // pw_stream_queue_buffer must run under the stream loop lock when called
	      // off the PipeWire thread. We never hold frame_mutex here, so there is no
	      // lock-order inversion with on_stream_process (which takes frame_mutex while
	      // the loop lock is held).
	      void requeue_buffer_from_consumer(struct pw_buffer *buf) {
	        if (!buf || !loop || !stream) {
	          return;
	        }
	        pw_thread_loop_lock(loop);
	        if (stream) {
	          pw_stream_queue_buffer(stream, buf);
	        }
	        pw_thread_loop_unlock(loop);
	      }

	      void trigger_pipewire_process() {
	        if (!gamescope_pipewire_capture || !stream) {
	          return;
	        }

	        const auto result = pw_stream_trigger_process(stream);
	        if (result < 0 && !logged_trigger_failure) {
	          logged_trigger_failure = true;
	          BOOST_LOG(warning) << "Unable to trigger Gamescope PipeWire graph processing: " << strerror(-result);
	        }
	      }

	      const spa_pod *build_gamescope_pipewire_format(
	        spa_pod_builder *builder,
	        bool dmabuf,
	        const spa_rectangle &requested_size,
	        const spa_rectangle &min_size,
	        const spa_fraction &requested_rate
	      ) {
	        spa_pod_frame frame {};
	        const spa_rectangle max_size {65535, 65535};
	        const spa_fraction min_rate {0, 1};
	        const std::int64_t focus_appid = 0;

	        spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	        spa_pod_builder_add(
	          builder,
	          SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
	          SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	          SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
	          SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&requested_size, &min_size, &max_size),
	          SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&requested_rate, &min_rate, &requested_rate),
	          GAMESCOPE_FORMAT_REQUESTED_SIZE, SPA_POD_Rectangle(&requested_size),
	          GAMESCOPE_FORMAT_FOCUS_APPID, SPA_POD_Long(focus_appid),
	          0
	        );

	        if (dmabuf) {
	          spa_pod_frame choice {};
	          if (!importable_modifiers_.empty()) {
	            spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
	            spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
	            // First element is the default; repeat the full importer list. Do NOT offer
	            // DRM_FORMAT_MOD_INVALID here: that lets the producer fixate to an implicit/INVALID
	            // modifier we cannot import. Force one of the explicit block-linear modifiers.
	            spa_pod_builder_long(builder, static_cast<std::int64_t>(importable_modifiers_[0]));
	            for (auto modifier : importable_modifiers_) {
	              spa_pod_builder_long(builder, static_cast<std::int64_t>(modifier));
	            }
	          } else {
	            // No importable modifiers discovered: keep the original LINEAR-only behavior.
	            spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
	            spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
	            spa_pod_builder_long(builder, DRM_FORMAT_MOD_LINEAR);
	            spa_pod_builder_long(builder, DRM_FORMAT_MOD_LINEAR);
	          }
	          spa_pod_builder_pop(builder, &choice);
	        }

	        return static_cast<const spa_pod *>(spa_pod_builder_pop(builder, &frame));
	      }

	      const spa_pod *build_pipewire_format(
	        spa_pod_builder *builder,
	        spa_video_format format,
	        bool dmabuf,
	        const spa_rectangle &requested_size,
	        const spa_rectangle &min_size,
	        const spa_fraction &no_fixed_rate,
	        const spa_fraction &requested_rate,
	        const spa_fraction &min_rate
	      ) {
	        spa_pod_frame frame {};
	        spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	        spa_pod_builder_add(
	          builder,
	          SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
	          SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	          SPA_FORMAT_VIDEO_format, SPA_POD_Id(format),
	          SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&requested_size, &min_size, &requested_size),
	          SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&no_fixed_rate),
	          SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&requested_rate, &min_rate, &requested_rate),
	          0
	        );

	        if (dmabuf) {
	          spa_pod_frame choice {};
	          spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
	          spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
	          if (!importable_modifiers_.empty()) {
	            // First element is the default; repeat the full importer list. Do NOT offer
	            // DRM_FORMAT_MOD_INVALID here: that lets the producer fixate to an implicit/INVALID
	            // modifier we cannot import. Force one of the explicit block-linear modifiers.
	            spa_pod_builder_long(builder, static_cast<std::int64_t>(importable_modifiers_[0]));
	            for (auto modifier : importable_modifiers_) {
	              spa_pod_builder_long(builder, static_cast<std::int64_t>(modifier));
	            }
	          } else {
	            // No importable modifiers discovered: keep the original LINEAR-only behavior.
	            spa_pod_builder_long(builder, DRM_FORMAT_MOD_LINEAR);
	            spa_pod_builder_long(builder, DRM_FORMAT_MOD_LINEAR);
	            spa_pod_builder_long(builder, DRM_FORMAT_MOD_INVALID);
	          }
	          spa_pod_builder_pop(builder, &choice);
	        }

	        return static_cast<const spa_pod *>(spa_pod_builder_pop(builder, &frame));
	      }

	      void log_process_diag(std::size_t bytes, double copy_ms, std::uint32_t data_type, std::int32_t stride) {
	        ++process_frames;
	        process_bytes += bytes;
	        process_max_copy_ms = std::max(process_max_copy_ms, copy_ms);

	        const auto now = std::chrono::steady_clock::now();
	        if (now - process_diag_at < 1s) {
	          return;
	        }

	        const auto elapsed = std::chrono::duration<double>(now - process_diag_at).count();
	        const auto mbps = elapsed > 0 ? (static_cast<double>(process_bytes) / (1024.0 * 1024.0)) / elapsed : 0.0;
	        BOOST_LOG(info) << "GNOME PipeWire process diag display=" << display_name
	                        << " frames=" << process_frames
	                        << " latest=" << latest_width << 'x' << latest_height
	                        << " data_type=" << data_type
	                        << " stride=" << stride
	                        << " mbps=" << mbps
	                        << " max_copy_ms=" << process_max_copy_ms;

	        process_diag_at = now;
	        process_frames = 0;
	        process_bytes = 0;
	        process_max_copy_ms = 0;
	      }

	      void log_capture_diag(bool copied, double wait_ms, double copy_ms) {
	        ++capture_frames;
	        if (copied) {
	          ++capture_new_frames;
	        } else {
	          ++capture_repeated_frames;
	        }
	        capture_max_wait_ms = std::max(capture_max_wait_ms, wait_ms);
	        capture_max_copy_ms = std::max(capture_max_copy_ms, copy_ms);

	        const auto now = std::chrono::steady_clock::now();
	        if (now - capture_diag_at < 1s) {
	          return;
	        }

	        BOOST_LOG(info) << "GNOME PipeWire capture diag display=" << display_name
	                        << " frames=" << capture_frames
	                        << " new=" << capture_new_frames
	                        << " repeated=" << capture_repeated_frames
	                        << " max_wait_ms=" << capture_max_wait_ms
	                        << " max_copy_ms=" << capture_max_copy_ms;

	        capture_diag_at = now;
	        capture_frames = 0;
	        capture_new_frames = 0;
	        capture_repeated_frames = 0;
	        capture_max_wait_ms = 0;
	        capture_max_copy_ms = 0;
	      }

	      void stop() {
        running = false;
        frame_cv.notify_all();
        node_cv.notify_all();

        if (owns_mutter_session && stream_path.size()) {
          call_no_args(stream_path, "org.gnome.Mutter.ScreenCast.Stream", "Stop");
        }
        if (owns_mutter_session && session_path.size()) {
          call_no_args(session_path, "org.gnome.Mutter.ScreenCast.Session", "Stop");
        }

        if (bus && signal_id) {
          g_dbus_connection_signal_unsubscribe(bus, signal_id);
          signal_id = 0;
        }

        if (loop) {
          pw_thread_loop_stop(loop);
          pw_thread_loop_lock(loop);
        }
        if (stream) {
          pw_stream_destroy(stream);
          stream = nullptr;
        }
        if (core) {
          pw_core_disconnect(core);
          core = nullptr;
        }
        if (context) {
          pw_context_destroy(context);
          context = nullptr;
        }
        if (loop) {
          pw_thread_loop_unlock(loop);
          pw_thread_loop_destroy(loop);
          loop = nullptr;
        }
        if (bus) {
          g_object_unref(bus);
          bus = nullptr;
        }
        if (encode_drm_fd_ >= 0) {
          close(encode_drm_fd_);
          encode_drm_fd_ = -1;
        }
      }

      std::string display_name;
      mem_type_e mem_type {};
      std::uint32_t framerate {};
      std::chrono::nanoseconds frame_interval {16ms};
      std::atomic<bool> running {false};

      GDBusConnection *bus {};
      std::string session_path;
      std::string stream_path;
      bool owns_mutter_session {true};
      guint signal_id {};
      std::mutex node_mutex;
      std::condition_variable node_cv;
      guint node_id {};

      pw_thread_loop *loop {};
      pw_context *context {};
      pw_core *core {};
      pw_stream *stream {};
      pw_stream_flags stream_flags {};

	      std::mutex frame_mutex;
	      std::condition_variable format_cv;
	      std::condition_variable frame_cv;
		      std::vector<std::uint8_t> latest_pixels;
	      dmabuf_frame_t latest_dmabuf;
		      int latest_width {};
		      int latest_height {};
		      int latest_stride {};
		      std::uint64_t latest_generation {};
		      std::uint64_t consumed_generation {};
		      std::uint64_t consumed_cursor_serial {};
		      std::chrono::steady_clock::time_point latest_timestamp {};
		      spa_video_format pipewire_format {SPA_VIDEO_FORMAT_UNKNOWN};
	      std::uint64_t pipewire_modifier {DRM_FORMAT_MOD_INVALID};
	      std::vector<std::uint64_t> importable_modifiers_;
	      bool dmabuf_modifier_fixated {false};
	      VDISPLAY::PIPEWIRE_DMABUF dmabuf_policy {VDISPLAY::PIPEWIRE_DMABUF::OFF};
	      pipewire_capture_mode_e active_capture_mode {pipewire_capture_mode_e::MAPPED};
	      bool dmabuf_allowed {};
	      bool dmabuf_policy_error {};
	      // Sticky (per-display): once a runtime DMA-BUF import fails under AUTO we
	      // trigger the mapped fallback exactly once. Guards against repeated triggers.
	      std::atomic<bool> dmabuf_disabled_for_session_ {false};
	      // Set when fall_back_to_mapped_capture() wants the capture loop to return
	      // capture_e::reinit so the video core rebuilds the session in mapped mode.
	      std::atomic<bool> request_mapped_reinit_ {false};
	      // Owned render-node fd for the resolved DMA-BUF encode/import GPU (-1 if none).
	      int encode_drm_fd_ {-1};
	      bool format_ready {};
	      bool format_failed {};
      bool logged_unexpected_unmapped_dmabuf {};
      bool gamescope_drives_pipewire {};
      bool logged_trigger_failure {};
      bool gamescope_pipewire_capture {};
      bool is_mutter_pipewire {};
      std::atomic<bool> mutter_layout_applied {false};
      bool logged_software_cursor_overlay {};
		      std::chrono::steady_clock::time_point process_diag_at {};
	      std::uint64_t process_frames {};
	      std::uint64_t process_bytes {};
	      double process_max_copy_ms {};
	      std::chrono::steady_clock::time_point capture_diag_at {};
	      std::uint64_t capture_frames {};
	      std::uint64_t capture_new_frames {};
	      std::uint64_t capture_repeated_frames {};
	      double capture_max_wait_ms {};
	      double capture_max_copy_ms {};
	    };
  }  // namespace

  std::shared_ptr<display_t> pipewire_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != mem_type_e::system && hwdevice_type != mem_type_e::vaapi && hwdevice_type != mem_type_e::cuda) {
      BOOST_LOG(debug) << "GNOME PipeWire capture cannot initialize with this memory type."sv;
      return nullptr;
    }

    auto display = std::make_shared<pipewire_display_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }

  std::vector<std::string> pipewire_display_names() {
    if (!mutter_screencast_available()) {
      return {};
    }

    return {"pipewire-virtual"};
  }
}  // namespace platf

#endif

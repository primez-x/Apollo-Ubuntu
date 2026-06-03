/**
 * @file src/platform/linux/virtual_display.cpp
 * @brief Virtual display implementation for Linux.
 *
 * The default Wayland backend uses GNOME Mutter RecordVirtual to provide a
 * virtual monitor and captures it via Mutter ScreenCast/PipeWire. An optional
 * Gamescope/PipeWire backend is available through the
 * linux_virtual_display_backend config key. APOLLO_LINUX_VIRTUAL_BACKEND is
 * kept as an explicit developer override for diagnostics and bisection.
 */

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

// platform includes
#include <drm/drm.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifdef SUNSHINE_BUILD_LIBEI
  #include <libei.h>
#endif

#ifdef SUNSHINE_BUILD_PIPEWIRE
  #include <gio/gio.h>
#endif

// local includes
#include "misc.h"
#include "mutter_dbus.h"
#include "src/config.h"
#include "src/logging.h"
#include "virtual_display.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace VDISPLAY {

  // ============================================================================
  // Global State
  // ============================================================================

  static std::mutex vdisplay_mutex;
  static DRIVER_STATUS driver_status = DRIVER_STATUS::UNKNOWN;
  static std::atomic<bool> watchdog_running {false};
  static std::thread watchdog_thread;
  static BACKEND selected_backend = BACKEND::UNKNOWN;

  static std::string xdg_runtime_path() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) {
      return runtime;
    }

    return "/run/user/" + std::to_string(getuid());
  }

  static bool command_in_path(const char *command) {
    const char *path_env = std::getenv("PATH");
    if (!path_env || !*path_env) {
      path_env = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }

    std::stringstream path_stream(path_env);
    std::string dir;
    while (std::getline(path_stream, dir, ':')) {
      if (dir.empty()) {
        dir = ".";
      }
      auto candidate = fs::path(dir) / command;
      if (::access(candidate.c_str(), X_OK) == 0) {
        return true;
      }
    }

    return false;
  }

  static std::string gamescope_binary() {
    const char *override = std::getenv("APOLLO_GAMESCOPE_BINARY");
    if (override && *override) {
      BOOST_LOG(info) << "[VDISPLAY] APOLLO_GAMESCOPE_BINARY override is active: " << override;
      return override;
    }

    return "gamescope";
  }

  static bool gamescope_binary_available(const std::string &binary) {
    if (binary.find('/') != std::string::npos) {
      return ::access(binary.c_str(), X_OK) == 0;
    }

    return command_in_path(binary.c_str());
  }

#ifdef SUNSHINE_BUILD_LIBEI
  static std::optional<uint32_t> moonlight_vk_to_evdev(uint16_t modcode) {
    switch (modcode) {
      case 0x08:
        return KEY_BACKSPACE;
      case 0x09:
        return KEY_TAB;
      case 0x0D:
        return KEY_ENTER;
      case 0x10:
      case 0xA0:
        return KEY_LEFTSHIFT;
      case 0x11:
      case 0xA2:
        return KEY_LEFTCTRL;
      case 0x14:
        return KEY_CAPSLOCK;
      case 0x1B:
        return KEY_ESC;
      case 0x20:
        return KEY_SPACE;
      case 0x21:
        return KEY_PAGEUP;
      case 0x22:
        return KEY_PAGEDOWN;
      case 0x23:
        return KEY_END;
      case 0x24:
        return KEY_HOME;
      case 0x25:
        return KEY_LEFT;
      case 0x26:
        return KEY_UP;
      case 0x27:
        return KEY_RIGHT;
      case 0x28:
        return KEY_DOWN;
      case 0x2C:
        return KEY_SYSRQ;
      case 0x2D:
        return KEY_INSERT;
      case 0x2E:
        return KEY_DELETE;
      case 0x30:
        return KEY_0;
      case 0x31:
        return KEY_1;
      case 0x32:
        return KEY_2;
      case 0x33:
        return KEY_3;
      case 0x34:
        return KEY_4;
      case 0x35:
        return KEY_5;
      case 0x36:
        return KEY_6;
      case 0x37:
        return KEY_7;
      case 0x38:
        return KEY_8;
      case 0x39:
        return KEY_9;
      case 0x41:
        return KEY_A;
      case 0x42:
        return KEY_B;
      case 0x43:
        return KEY_C;
      case 0x44:
        return KEY_D;
      case 0x45:
        return KEY_E;
      case 0x46:
        return KEY_F;
      case 0x47:
        return KEY_G;
      case 0x48:
        return KEY_H;
      case 0x49:
        return KEY_I;
      case 0x4A:
        return KEY_J;
      case 0x4B:
        return KEY_K;
      case 0x4C:
        return KEY_L;
      case 0x4D:
        return KEY_M;
      case 0x4E:
        return KEY_N;
      case 0x4F:
        return KEY_O;
      case 0x50:
        return KEY_P;
      case 0x51:
        return KEY_Q;
      case 0x52:
        return KEY_R;
      case 0x53:
        return KEY_S;
      case 0x54:
        return KEY_T;
      case 0x55:
        return KEY_U;
      case 0x56:
        return KEY_V;
      case 0x57:
        return KEY_W;
      case 0x58:
        return KEY_X;
      case 0x59:
        return KEY_Y;
      case 0x5A:
        return KEY_Z;
      case 0x5B:
        return KEY_LEFTMETA;
      case 0x5C:
        return KEY_RIGHTMETA;
      case 0x60:
        return KEY_KP0;
      case 0x61:
        return KEY_KP1;
      case 0x62:
        return KEY_KP2;
      case 0x63:
        return KEY_KP3;
      case 0x64:
        return KEY_KP4;
      case 0x65:
        return KEY_KP5;
      case 0x66:
        return KEY_KP6;
      case 0x67:
        return KEY_KP7;
      case 0x68:
        return KEY_KP8;
      case 0x69:
        return KEY_KP9;
      case 0x6A:
        return KEY_KPASTERISK;
      case 0x6B:
        return KEY_KPPLUS;
      case 0x6D:
        return KEY_KPMINUS;
      case 0x6E:
        return KEY_KPDOT;
      case 0x6F:
        return KEY_KPSLASH;
      case 0x70:
        return KEY_F1;
      case 0x71:
        return KEY_F2;
      case 0x72:
        return KEY_F3;
      case 0x73:
        return KEY_F4;
      case 0x74:
        return KEY_F5;
      case 0x75:
        return KEY_F6;
      case 0x76:
        return KEY_F7;
      case 0x77:
        return KEY_F8;
      case 0x78:
        return KEY_F9;
      case 0x79:
        return KEY_F10;
      case 0x7A:
        return KEY_F11;
      case 0x7B:
        return KEY_F12;
      case 0x90:
        return KEY_NUMLOCK;
      case 0x91:
        return KEY_SCROLLLOCK;
      case 0xA1:
        return KEY_RIGHTSHIFT;
      case 0xA3:
        return KEY_RIGHTCTRL;
      case 0xA4:
        return KEY_LEFTALT;
      case 0xA5:
        return KEY_RIGHTALT;
      case 0xBA:
        return KEY_SEMICOLON;
      case 0xBB:
        return KEY_EQUAL;
      case 0xBC:
        return KEY_COMMA;
      case 0xBD:
        return KEY_MINUS;
      case 0xBE:
        return KEY_DOT;
      case 0xBF:
        return KEY_SLASH;
      case 0xC0:
        return KEY_GRAVE;
      case 0xDB:
        return KEY_LEFTBRACE;
      case 0xDC:
        return KEY_BACKSLASH;
      case 0xDD:
        return KEY_RIGHTBRACE;
      case 0xDE:
        return KEY_APOSTROPHE;
      case 0xE2:
        return KEY_102ND;
      default:
        return std::nullopt;
    }
  }

  class gamescope_ei_client_t {
  public:
    ~gamescope_ei_client_t() {
      stop();
    }

    bool start(const std::string &socket_path) {
      std::lock_guard lock(mutex);
      if (ctx) {
        return true;
      }

      ctx = ei_new_sender(nullptr);
      if (!ctx) {
        BOOST_LOG(error) << "[VDISPLAY] Failed to create Gamescope EI sender.";
        return false;
      }

      ei_configure_name(ctx, "Apollo Gamescope Input");
      int rc = ei_setup_backend_socket(ctx, socket_path.c_str());
      if (rc < 0) {
        BOOST_LOG(error) << "[VDISPLAY] Failed to connect to Gamescope EI socket [" << socket_path << "]: " << strerror(-rc);
        ei_unref(ctx);
        ctx = nullptr;
        return false;
      }

      running = true;
      thread = std::thread([this]() {
        event_loop();
      });
      return true;
    }

    void stop() {
      {
        std::lock_guard lock(mutex);
        running = false;
      }

      if (thread.joinable()) {
        thread.join();
      }

      std::lock_guard lock(mutex);
      pointer_device = unref_device(pointer_device);
      keyboard_device = unref_device(keyboard_device);
      if (ctx) {
        ei_unref(ctx);
        ctx = nullptr;
      }
      emulating_devices.clear();
    }

    bool pointer_motion_relative(double dx, double dy) {
      std::lock_guard lock(mutex);
      if (!ready_for(EI_DEVICE_CAP_POINTER)) {
        return false;
      }

      ensure_emulating(pointer_device);
      ei_device_pointer_motion(pointer_device, dx, dy);
      ei_device_frame(pointer_device, ei_now(ctx));
      return true;
    }

    bool pointer_motion_absolute(double x, double y) {
      std::lock_guard lock(mutex);
      if (!ready_for(EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
        return false;
      }

      ensure_emulating(pointer_device);
      ei_device_pointer_motion_absolute(pointer_device, x, y);
      ei_device_frame(pointer_device, ei_now(ctx));
      return true;
    }

    bool pointer_button(int button, bool release) {
      std::lock_guard lock(mutex);
      if (!ready_for(EI_DEVICE_CAP_BUTTON)) {
        return false;
      }

      ensure_emulating(pointer_device);
      ei_device_button_button(pointer_device, static_cast<uint32_t>(button), !release);
      ei_device_frame(pointer_device, ei_now(ctx));
      return true;
    }

    bool pointer_axis(double dx, double dy) {
      std::lock_guard lock(mutex);
      if (!ready_for(EI_DEVICE_CAP_SCROLL)) {
        return false;
      }

      ensure_emulating(pointer_device);
      ei_device_scroll_delta(pointer_device, dx, dy);
      ei_device_frame(pointer_device, ei_now(ctx));
      return true;
    }

    bool keyboard_key(uint16_t modcode, bool release) {
      auto key = moonlight_vk_to_evdev(modcode);
      if (!key) {
        return false;
      }

      std::lock_guard lock(mutex);
      if (!keyboard_device || !keyboard_ready) {
        return false;
      }

      ensure_emulating(keyboard_device);
      ei_device_keyboard_key(keyboard_device, *key, !release);
      ei_device_frame(keyboard_device, ei_now(ctx));
      return true;
    }

  private:
    static ei_device *unref_device(ei_device *device) {
      if (device) {
        ei_device_unref(device);
      }
      return nullptr;
    }

    bool ready_for(ei_device_capability capability) const {
      return ctx && pointer_device && pointer_ready && ei_device_has_capability(pointer_device, capability);
    }

    void ensure_emulating(ei_device *device) {
      if (device && emulating_devices.insert(device).second) {
        ei_device_start_emulating(device, ++sequence);
      }
    }

    void event_loop() {
      while (true) {
        {
          std::lock_guard lock(mutex);
          if (!running || !ctx) {
            break;
          }
        }

        int fd {};
        {
          std::lock_guard lock(mutex);
          fd = ei_get_fd(ctx);
        }

        pollfd pfd {fd, POLLIN, 0};
        poll(&pfd, 1, 100);

        std::lock_guard lock(mutex);
        if (!running || !ctx) {
          break;
        }

        ei_dispatch(ctx);
        while (auto *event = ei_get_event(ctx)) {
          handle_event(event);
          ei_event_unref(event);
        }
      }
    }

    void handle_event(ei_event *event) {
      const auto type = ei_event_get_type(event);
      switch (type) {
        case EI_EVENT_SEAT_ADDED: {
          auto *seat = ei_event_get_seat(event);
          ei_seat_bind_capabilities(
            seat,
            EI_DEVICE_CAP_POINTER_ABSOLUTE,
            EI_DEVICE_CAP_POINTER,
            EI_DEVICE_CAP_BUTTON,
            EI_DEVICE_CAP_SCROLL,
            EI_DEVICE_CAP_KEYBOARD,
            nullptr
          );
          break;
        }
        case EI_EVENT_DEVICE_ADDED: {
          auto *device = ei_event_get_device(event);
          if (!pointer_device &&
              ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE) &&
              ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON)) {
            pointer_device = ei_device_ref(device);
          }
          if (!keyboard_device && ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
            keyboard_device = ei_device_ref(device);
          }
          break;
        }
        case EI_EVENT_DEVICE_RESUMED: {
          auto *device = ei_event_get_device(event);
          if (device == pointer_device) {
            pointer_ready = true;
            BOOST_LOG(info) << "[VDISPLAY] Gamescope EI pointer device is ready.";
          }
          if (device == keyboard_device) {
            keyboard_ready = true;
            BOOST_LOG(info) << "[VDISPLAY] Gamescope EI keyboard device is ready.";
          }
          break;
        }
        case EI_EVENT_DISCONNECT:
          running = false;
          break;
        default:
          break;
      }
    }

    std::mutex mutex;
    std::thread thread;
    ei *ctx {};
    ei_device *pointer_device {};
    ei_device *keyboard_device {};
    bool running {};
    bool pointer_ready {};
    bool keyboard_ready {};
    std::set<ei_device *> emulating_devices;
    uint32_t sequence {};
  };
#else
  class gamescope_ei_client_t {
  public:
    bool start(const std::string &) {
      BOOST_LOG(warning) << "[VDISPLAY] Gamescope EI input is unavailable because Apollo was built without libei.";
      return false;
    }
    void stop() {}
    bool pointer_motion_relative(double, double) { return false; }
    bool pointer_motion_absolute(double, double) { return false; }
    bool pointer_button(int, bool) { return false; }
    bool pointer_axis(double, double) { return false; }
    bool keyboard_key(uint16_t, bool) { return false; }
  };
#endif

  class gamescope_session_t {
  public:
    ~gamescope_session_t() {
      stop();
    }

    bool start(const std::string &display_name, uint32_t width, uint32_t height, uint32_t fps_hz) {
      auto binary = gamescope_binary();
      if (!gamescope_binary_available(binary)) {
        BOOST_LOG(error) << "[VDISPLAY] Gamescope backend requested, but Gamescope binary is not executable: " << binary;
        return false;
      }

      int pipe_fds[2] {-1, -1};
      if (pipe(pipe_fds) < 0) {
        BOOST_LOG(error) << "[VDISPLAY] Failed to create Gamescope log pipe: " << strerror(errno);
        return false;
      }

      std::string width_arg = std::to_string(width);
      std::string height_arg = std::to_string(height);
      std::string refresh_arg = std::to_string(std::max<uint32_t>(1, fps_hz));
      auto command = session_command();
      auto extra_args = gamescope_extra_args();

      pid = fork();
      if (pid < 0) {
        BOOST_LOG(error) << "[VDISPLAY] Failed to fork Gamescope: " << strerror(errno);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return false;
      }

      if (pid == 0) {
        setsid();
        ::close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[1]);

        std::vector<std::string> args {
          binary,
          "--backend",
          "headless",
          "--keep-alive",
          "--expose-wayland",
          "-W",
          width_arg,
          "-H",
          height_arg,
          "-w",
          width_arg,
          "-h",
          height_arg,
          "-r",
          refresh_arg,
          "--force-windows-fullscreen"
        };
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        args.insert(args.end(), {"--", "/bin/sh", "-lc", command});

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (auto &arg : args) {
          argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        execvp(binary.c_str(), argv.data());
        _exit(127);
      }

      ::close(pipe_fds[1]);
      log_thread = std::thread([this, fd = pipe_fds[0]]() {
        read_log(fd);
      });

      std::unique_lock lock(mutex);
      bool node_ready = cv.wait_for(lock, 7s, [this]() {
        return (pipewire_node_id != 0 && !wayland_display.empty()) || exited;
      });
      if (!node_ready || pipewire_node_id == 0) {
        lock.unlock();
        BOOST_LOG(error) << "[VDISPLAY] Timed out waiting for Gamescope PipeWire node for " << display_name;
        stop();
        return false;
      }

      if (ei_socket.empty()) {
        ei_socket = xdg_runtime_path() + "/" + wayland_display + "-ei";
      }
      auto socket_path = ei_socket;
      auto node = pipewire_node_id;
      auto launch_env = launch_environment_locked();
      lock.unlock();

      ei_client = std::make_shared<gamescope_ei_client_t>();
      if (!ei_client->start(socket_path)) {
        BOOST_LOG(warning) << "[VDISPLAY] Gamescope input will fall back to the host input path.";
      }

      BOOST_LOG(info) << "[VDISPLAY] Gamescope session for " << display_name
                      << " is ready: pipewire_node=" << node
                      << " wayland_display=" << launch_env.wayland_display
                      << " x11_display=" << (launch_env.x11_display.empty() ? "(pending)" : launch_env.x11_display)
                      << " ei_socket=" << socket_path;
      return true;
    }

    void stop() {
      if (ei_client) {
        ei_client->stop();
        ei_client.reset();
      }

      pid_t local_pid = pid.exchange(-1);
      if (local_pid > 0) {
        ::kill(-local_pid, SIGTERM);
        for (int attempt = 0; attempt < 20; ++attempt) {
          int status {};
          auto result = waitpid(local_pid, &status, WNOHANG);
          if (result == local_pid || result < 0) {
            local_pid = -1;
            break;
          }
          std::this_thread::sleep_for(100ms);
        }
        if (local_pid > 0) {
          ::kill(-local_pid, SIGKILL);
          waitpid(local_pid, nullptr, 0);
        }
      }

      if (log_thread.joinable()) {
        log_thread.join();
      }
    }

    uint32_t node_id() const {
      std::lock_guard lock(mutex);
      return pipewire_node_id;
    }

    std::shared_ptr<gamescope_ei_client_t> input() const {
      return ei_client;
    }

    gamescope_launch_environment_t launch_environment() const {
      std::lock_guard lock(mutex);
      return launch_environment_locked();
    }

  private:
	    static std::string session_command() {
      const char *command_override = std::getenv("APOLLO_GAMESCOPE_COMMAND");
      if (command_override && *command_override) {
        BOOST_LOG(info) << "[VDISPLAY] APOLLO_GAMESCOPE_COMMAND override is active.";
        return command_override;
      }

      if (!config::video.linux_gamescope_session_command.empty()) {
        return config::video.linux_gamescope_session_command;
      }

      BOOST_LOG(info) << "[VDISPLAY] No Gamescope session command configured; using a sleep supervisor.";
	      return "sleep infinity";
	    }

	    static std::vector<std::string> gamescope_extra_args() {
	      const char *args_override = std::getenv("APOLLO_GAMESCOPE_FLAGS");
	      if (!args_override || !*args_override) {
	        return {};
	      }

	      BOOST_LOG(info) << "[VDISPLAY] APOLLO_GAMESCOPE_FLAGS override is active: " << args_override;
	      std::istringstream stream(args_override);
	      std::vector<std::string> args;
	      std::string arg;
	      while (stream >> arg) {
	        args.push_back(std::move(arg));
	      }
	      return args;
	    }

	    gamescope_launch_environment_t launch_environment_locked() const {
      return {
        wayland_display,
        x11_display,
        ei_socket
      };
    }

    void read_log(int fd) {
      FILE *stream = fdopen(fd, "r");
      if (!stream) {
        ::close(fd);
        return;
      }

      char *line = nullptr;
      size_t len = 0;
      while (getline(&line, &len, stream) != -1) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
          text.pop_back();
        }
        if (!text.empty()) {
          BOOST_LOG(info) << "[GAMESCOPE] " << text;
          parse_log_line(text);
        }
      }
      free(line);
      fclose(stream);

      {
        std::lock_guard lock(mutex);
        exited = true;
      }
      cv.notify_all();
    }

    void parse_log_line(const std::string &line) {
      constexpr std::string_view node_marker = "stream available on node ID: ";
      auto node_pos = line.find(node_marker);
      if (node_pos != std::string::npos) {
        auto value = line.substr(node_pos + node_marker.size());
        char *end {};
        auto node = std::strtoul(value.c_str(), &end, 10);
        if (node > 0) {
          {
            std::lock_guard lock(mutex);
            pipewire_node_id = static_cast<uint32_t>(node);
          }
          cv.notify_all();
        }
      }

      constexpr std::string_view display_marker = "Running compositor on wayland display '";
      auto display_pos = line.find(display_marker);
      if (display_pos != std::string::npos) {
        auto start = display_pos + display_marker.size();
        auto end = line.find('\'', start);
        if (end != std::string::npos && end > start) {
          auto wayland_display = line.substr(start, end - start);
          {
            std::lock_guard lock(mutex);
            this->wayland_display = wayland_display;
            ei_socket = xdg_runtime_path() + "/" + wayland_display + "-ei";
          }
          cv.notify_all();
        }
      }

      constexpr std::string_view xwayland_marker = "Starting Xwayland on ";
      auto xwayland_pos = line.find(xwayland_marker);
      if (xwayland_pos != std::string::npos) {
        auto value = line.substr(xwayland_pos + xwayland_marker.size());
        auto end = value.find_first_of(" \t\r\n");
        if (end != std::string::npos) {
          value.resize(end);
        }
        if (!value.empty()) {
          std::lock_guard lock(mutex);
          x11_display = value;
        }
      }
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<pid_t> pid {-1};
    std::thread log_thread;
    uint32_t pipewire_node_id {};
    std::string wayland_display;
    std::string x11_display;
    std::string ei_socket;
    bool exited {};
    std::shared_ptr<gamescope_ei_client_t> ei_client;
  };

  // Virtual display info structure
  struct VirtualDisplayInfo {
    std::string name;
    std::string guid_str;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    BACKEND backend;
    int drm_fd;            // DRM fd for card
    bool active;
    std::string saved_primary_connector;  // physical primary to restore on Mutter/PipeWire teardown
    bool gamescope_cursor_overlay = true;
    std::shared_ptr<gamescope_session_t> gamescope;
    gamescope_cursor_state_t gamescope_cursor;
#ifdef SUNSHINE_BUILD_PIPEWIRE
    GDBusConnection *mutter_bus {};
    std::string mutter_remote_desktop_session_path;
    std::string mutter_remote_desktop_session_id;
    std::string mutter_session_path;
    std::string mutter_stream_path;
    uint32_t pipewire_node_id {};
#endif
  };

  static std::map<std::string, VirtualDisplayInfo> virtual_displays;

  static std::string lower_copy(std::string value) {
    std::transform(std::begin(value), std::end(value), std::begin(value), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return value;
  }

  static std::string normalized_backend_token(std::string_view value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
      return std::isspace(c);
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
      return std::isspace(c);
    }).base();
    if (begin >= end) {
      return {};
    }

    return lower_copy(std::string(begin, end));
  }

  std::optional<BACKEND> parseLinuxVirtualDisplayBackend(std::string_view value) {
    auto backend = normalized_backend_token(value);
    if (backend.empty()) {
      return std::nullopt;
    }

    if (backend == "auto" || backend == "hybrid" || backend == "mutter" || backend == "mutter_pipewire" || backend == "mutter-pipewire" || backend == "pipewire") {
      return BACKEND::MUTTER_PIPEWIRE;
    }
    if (backend == "gamescope" || backend == "gamescope_pipewire" || backend == "gamescope-pipewire" || backend == "remote_session" || backend == "remote-session") {
      return BACKEND::GAMESCOPE_PIPEWIRE;
    }

    return std::nullopt;
  }

  BACKEND resolveLinuxVirtualDisplayBackend(std::string_view config_value, const char *environment_override) {
    if (environment_override && *environment_override) {
      if (auto backend = parseLinuxVirtualDisplayBackend(environment_override)) {
        return *backend;
      }
    }

    if (auto backend = parseLinuxVirtualDisplayBackend(config_value)) {
      return *backend;
    }

    return BACKEND::MUTTER_PIPEWIRE;
  }

  std::optional<CAPTURE_BACKEND> parseLinuxVirtualCaptureBackend(std::string_view value) {
    auto backend = normalized_backend_token(value);
    if (backend.empty()) {
      return std::nullopt;
    }

    if (backend == "auto") {
      return CAPTURE_BACKEND::AUTO;
    }
    if (backend == "pipewire" || backend == "mutter" || backend == "mutter_pipewire" || backend == "mutter-pipewire") {
      return CAPTURE_BACKEND::PIPEWIRE;
    }
    if (backend == "nvidia" || backend == "nvfbc" || backend == "nvidia_capture" || backend == "nvidia-capture") {
      return CAPTURE_BACKEND::NVIDIA;
    }

    return std::nullopt;
  }

  CAPTURE_BACKEND resolveLinuxVirtualCaptureBackend(std::string_view config_value, const char *environment_override) {
    if (environment_override && *environment_override) {
      if (auto backend = parseLinuxVirtualCaptureBackend(environment_override)) {
        return *backend;
      }
    }

    if (auto backend = parseLinuxVirtualCaptureBackend(config_value)) {
      return *backend;
    }

    return CAPTURE_BACKEND::AUTO;
  }

  const char *linuxVirtualCaptureBackendName(CAPTURE_BACKEND backend) {
    switch (backend) {
      case CAPTURE_BACKEND::AUTO:
        return "auto";
      case CAPTURE_BACKEND::PIPEWIRE:
        return "PipeWire";
      case CAPTURE_BACKEND::NVIDIA:
        return "NVIDIA";
      default:
        return "unknown";
    }
  }

  std::optional<PIPEWIRE_DMABUF> parseLinuxPipeWireDmaBuf(std::string_view value) {
    auto mode = normalized_backend_token(value);
    if (mode.empty()) {
      return std::nullopt;
    }

    if (mode == "auto") {
      return PIPEWIRE_DMABUF::AUTO;
    }
    if (mode == "off" || mode == "false" || mode == "disabled" || mode == "disable" || mode == "0") {
      return PIPEWIRE_DMABUF::OFF;
    }
    if (mode == "force" || mode == "forced" || mode == "on" || mode == "true" || mode == "enabled" || mode == "enable" || mode == "1") {
      return PIPEWIRE_DMABUF::FORCE;
    }

    return std::nullopt;
  }

  PIPEWIRE_DMABUF resolveLinuxPipeWireDmaBuf(std::string_view config_value, const char *environment_override) {
    if (environment_override && *environment_override) {
      if (auto mode = parseLinuxPipeWireDmaBuf(environment_override)) {
        return *mode;
      }
    }

    if (auto mode = parseLinuxPipeWireDmaBuf(config_value)) {
      return *mode;
    }

    return PIPEWIRE_DMABUF::OFF;
  }

  const char *linuxPipeWireDmaBufName(PIPEWIRE_DMABUF mode) {
    switch (mode) {
      case PIPEWIRE_DMABUF::AUTO:
        return "auto";
      case PIPEWIRE_DMABUF::OFF:
        return "off";
      case PIPEWIRE_DMABUF::FORCE:
        return "force";
      default:
        return "unknown";
    }
  }

  const char *linuxVirtualDisplayBackendName(BACKEND backend) {
    switch (backend) {
      case BACKEND::MUTTER_PIPEWIRE:
        return "Mutter RecordVirtual/PipeWire";
      case BACKEND::GAMESCOPE_PIPEWIRE:
        return "Gamescope headless/PipeWire";
      case BACKEND::UNKNOWN:
      default:
        return "unknown";
    }
  }

  static BACKEND configured_backend() {
    const char *environment_override = std::getenv("APOLLO_LINUX_VIRTUAL_BACKEND");
    if (environment_override && *environment_override) {
      if (parseLinuxVirtualDisplayBackend(environment_override)) {
        BOOST_LOG(info) << "[VDISPLAY] APOLLO_LINUX_VIRTUAL_BACKEND override is active.";
      } else {
        BOOST_LOG(warning) << "[VDISPLAY] Unknown APOLLO_LINUX_VIRTUAL_BACKEND=" << environment_override
                           << "; ignoring environment override.";
      }
    }

    if (!config::video.linux_virtual_display_backend.empty() &&
        !parseLinuxVirtualDisplayBackend(config::video.linux_virtual_display_backend)) {
      BOOST_LOG(warning) << "[VDISPLAY] Unknown linux_virtual_display_backend="
                         << config::video.linux_virtual_display_backend
                         << "; defaulting to Mutter RecordVirtual/PipeWire.";
    }

    return resolveLinuxVirtualDisplayBackend(config::video.linux_virtual_display_backend, environment_override);
  }

#ifdef SUNSHINE_BUILD_PIPEWIRE
  namespace mutter_dbus = platf::mutter_dbus;

  namespace {
    struct mutter_node_wait_t {
      std::mutex mutex;
      std::condition_variable cv;
      uint32_t node_id {};
    };
  }  // namespace

  static bool mutter_screencast_available() {
    if (!std::getenv("WAYLAND_DISPLAY")) {
      BOOST_LOG(error) << "[VDISPLAY] Mutter/PipeWire virtual display requires a Wayland session.";
      return false;
    }

    GError *raw_error = nullptr;
    auto bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &raw_error);
    mutter_dbus::gerror_ptr dbus_error(raw_error);
    if (!bus) {
      BOOST_LOG(error) << "[VDISPLAY] Unable to connect to the session bus for Mutter/PipeWire virtual display: "
                       << (dbus_error ? dbus_error->message : "unknown");
      return false;
    }

    if (!mutter_dbus::name_has_owner(bus, mutter_dbus::SCREENCAST_SERVICE, mutter_dbus::QUICK_CALL_TIMEOUT_MS)) {
      BOOST_LOG(error) << "[VDISPLAY] Mutter ScreenCast service is not available.";
      g_object_unref(bus);
      return false;
    }

    raw_error = nullptr;
    auto version_result = mutter_dbus::call_sync(
      bus,
      mutter_dbus::SCREENCAST_SERVICE,
      mutter_dbus::SCREENCAST_PATH,
      "org.freedesktop.DBus.Properties",
      "Get",
      g_variant_new("(ss)", "org.gnome.Mutter.ScreenCast", "Version"),
      G_VARIANT_TYPE("(v)"),
      mutter_dbus::QUICK_CALL_TIMEOUT_MS,
      &raw_error
    );
    dbus_error.reset(raw_error);
    if (!version_result) {
      BOOST_LOG(error) << "[VDISPLAY] Unable to query Mutter ScreenCast version: "
                       << (dbus_error ? dbus_error->message : "unknown");
      g_object_unref(bus);
      return false;
    }

    GVariant *version_value = nullptr;
    g_variant_get(version_result, "(v)", &version_value);
    const auto version = mutter_dbus::uint32_from_variant(version_value);
    if (version_value) {
      g_variant_unref(version_value);
    }
    g_variant_unref(version_result);
    g_object_unref(bus);

    if (version < 4) {
      BOOST_LOG(error) << "[VDISPLAY] Mutter ScreenCast version " << version
                       << " does not support the Apollo virtual display backend.";
      return false;
    }

    BOOST_LOG(info) << "[VDISPLAY] Mutter ScreenCast version " << version
                    << " is available for virtual displays.";
    return true;
  }

  static bool call_dbus_no_args(VirtualDisplayInfo &vdinfo, const char *destination, const std::string &path, const char *interface, const char *method) {
    if (!vdinfo.mutter_bus || path.empty()) {
      return true;
    }

    GError *raw_error = nullptr;
    auto result = mutter_dbus::call_sync(
      vdinfo.mutter_bus,
      destination,
      path.c_str(),
      interface,
      method,
      nullptr,
      nullptr,
      mutter_dbus::QUICK_CALL_TIMEOUT_MS,
      &raw_error
    );
    mutter_dbus::gerror_ptr dbus_error(raw_error);
    if (!result) {
      if (mutter_dbus::error_is_missing_object(dbus_error.get())) {
        BOOST_LOG(debug) << "[VDISPLAY] Mutter DBus " << method
                         << " skipped because the object already disappeared.";
        return true;
      }
      BOOST_LOG(error) << "[VDISPLAY] Mutter DBus " << method
                       << " failed for " << vdinfo.name << ": "
                       << (dbus_error ? dbus_error->message : "unknown");
      return false;
    }

    g_variant_unref(result);
    return true;
  }

  static void destroy_mutter_virtual_stream(VirtualDisplayInfo &vdinfo) {
    if (!vdinfo.mutter_bus) {
      return;
    }

    if (!vdinfo.mutter_remote_desktop_session_path.empty()) {
      call_dbus_no_args(
        vdinfo,
        mutter_dbus::REMOTE_DESKTOP_SERVICE,
        vdinfo.mutter_remote_desktop_session_path,
        "org.gnome.Mutter.RemoteDesktop.Session",
        "Stop"
      );
    } else {
      call_dbus_no_args(vdinfo, mutter_dbus::SCREENCAST_SERVICE, vdinfo.mutter_stream_path, "org.gnome.Mutter.ScreenCast.Stream", "Stop");
      call_dbus_no_args(vdinfo, mutter_dbus::SCREENCAST_SERVICE, vdinfo.mutter_session_path, "org.gnome.Mutter.ScreenCast.Session", "Stop");
    }

    g_object_unref(vdinfo.mutter_bus);
    vdinfo.mutter_bus = nullptr;
    vdinfo.mutter_remote_desktop_session_path.clear();
    vdinfo.mutter_remote_desktop_session_id.clear();
    vdinfo.mutter_stream_path.clear();
    vdinfo.mutter_session_path.clear();
    vdinfo.pipewire_node_id = 0;
  }

  static bool create_mutter_remote_desktop_session(VirtualDisplayInfo &vdinfo) {
    GError *raw_error = nullptr;
    auto session_result = mutter_dbus::call_sync(
      vdinfo.mutter_bus,
      mutter_dbus::REMOTE_DESKTOP_SERVICE,
      mutter_dbus::REMOTE_DESKTOP_PATH,
      "org.gnome.Mutter.RemoteDesktop",
      "CreateSession",
      nullptr,
      G_VARIANT_TYPE("(o)"),
      mutter_dbus::QUICK_CALL_TIMEOUT_MS,
      &raw_error
    );
    mutter_dbus::gerror_ptr dbus_error(raw_error);
    if (!session_result) {
      BOOST_LOG(error) << "[VDISPLAY] Unable to create Mutter RemoteDesktop session for " << vdinfo.name
                       << ": " << (dbus_error ? dbus_error->message : "unknown");
      return false;
    }

    const char *session_path_raw = nullptr;
    g_variant_get(session_result, "(&o)", &session_path_raw);
    vdinfo.mutter_remote_desktop_session_path = session_path_raw ? session_path_raw : "";
    g_variant_unref(session_result);

    raw_error = nullptr;
    auto session_id_result = mutter_dbus::call_sync(
      vdinfo.mutter_bus,
      mutter_dbus::REMOTE_DESKTOP_SERVICE,
      vdinfo.mutter_remote_desktop_session_path.c_str(),
      "org.freedesktop.DBus.Properties",
      "Get",
      g_variant_new("(ss)", "org.gnome.Mutter.RemoteDesktop.Session", "SessionId"),
      G_VARIANT_TYPE("(v)"),
      mutter_dbus::QUICK_CALL_TIMEOUT_MS,
      &raw_error
    );
    dbus_error.reset(raw_error);
    if (!session_id_result) {
      BOOST_LOG(error) << "[VDISPLAY] Unable to read Mutter RemoteDesktop SessionId for " << vdinfo.name
                       << ": " << (dbus_error ? dbus_error->message : "unknown");
      return false;
    }

    GVariant *session_id_value = nullptr;
    g_variant_get(session_id_result, "(v)", &session_id_value);
    if (session_id_value && g_variant_is_of_type(session_id_value, G_VARIANT_TYPE_STRING)) {
      vdinfo.mutter_remote_desktop_session_id = g_variant_get_string(session_id_value, nullptr);
    }
    if (session_id_value) {
      g_variant_unref(session_id_value);
    }
    g_variant_unref(session_id_result);

    if (vdinfo.mutter_remote_desktop_session_id.empty()) {
      BOOST_LOG(error) << "[VDISPLAY] Mutter RemoteDesktop session for " << vdinfo.name
                       << " did not expose a SessionId.";
      return false;
    }

    BOOST_LOG(info) << "[VDISPLAY] Created Mutter RemoteDesktop session for " << vdinfo.name
                    << " id=" << vdinfo.mutter_remote_desktop_session_id;
    return true;
  }

  // DRM/Mutter connector names use a restricted charset (e.g. DP-6, HDMI-A-1, Meta-0).
  // Reject anything else so a connector name can never be interpreted as shell syntax.
  static bool is_safe_connector_name(const std::string &name) {
    if (name.empty() || name.size() > 64) {
      return false;
    }
    for (unsigned char c : name) {
      const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-';
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // Return the private per-user runtime dir (mode 0700) for helper scripts and their output.
  // We deliberately do NOT fall back to world-writable /tmp: these files are executed/read, so a
  // predictable /tmp path is a symlink/TOCTOU vector. Fail closed instead -- callers treat an empty
  // result as "cannot run the helper". A host without XDG_RUNTIME_DIR has no GNOME session bus
  // either, so the Mutter helpers could not work regardless.
  static std::string apollo_helper_dir() {
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] == '/') {
      return std::string(xdg);
    }
    return std::string();
  }

  static std::string detect_mutter_primary_connector() {
    const std::string helper_dir = apollo_helper_dir();
    if (helper_dir.empty()) {
      BOOST_LOG(warning) << "[VDISPLAY] XDG_RUNTIME_DIR unavailable; skipping Mutter primary detection.";
      return std::string();
    }
    const std::string script_path = helper_dir + "/apollo-mutter-primary.py";
    const std::string out_path = helper_dir + "/apollo-mutter-primary.out";
    std::ofstream script(script_path, std::ios::trunc);
    if (!script) {
      return std::string();
    }
    script << R"PY(#!/usr/bin/env python3
import time

from gi.repository import Gio

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)


def is_virtual(connector, vendor, product):
    hay = (connector + " " + vendor + " " + product).upper()
    return (
        connector.startswith("Meta-")
        or "METAVENDOR" in hay
        or "APOLLO" in hay
        or "VDISP" in hay
        or "VIRTUAL REMOTE MONITOR" in hay
    )


for _attempt in range(8):
    try:
        state = bus.call_sync(
            "org.gnome.Mutter.DisplayConfig",
            "/org/gnome/Mutter/DisplayConfig",
            "org.gnome.Mutter.DisplayConfig",
            "GetCurrentState",
            None,
            None,
            Gio.DBusCallFlags.NONE,
            3000,
            None,
        ).unpack()
    except Exception:
        time.sleep(0.25)
        continue

    chosen = None
    for logical in state[2]:
        if not (logical[4] and logical[5]):
            continue
        connector, vendor, product, _serial = logical[5][0]
        if is_virtual(connector, vendor, product):
            continue
        chosen = connector
        break

    if chosen:
        print(chosen)
        break

    time.sleep(0.25)
)PY";
    script.close();

    std::string command = std::string("timeout 4s python3 ") + script_path + " > " + out_path + " 2>/dev/null";
    int rc = std::system(command.c_str());
    (void) rc;

    std::ifstream in(out_path);
    std::string connector;
    std::getline(in, connector);
    while (!connector.empty() && (connector.back() == '\n' || connector.back() == '\r' || connector.back() == ' ' || connector.back() == '\t')) {
      connector.pop_back();
    }
    if (!is_safe_connector_name(connector)) {
      if (!connector.empty()) {
        BOOST_LOG(warning) << "[VDISPLAY] Ignoring unexpected primary connector name from Mutter.";
      }
      return std::string();
    }
    return connector;
  }

  static bool create_mutter_virtual_stream(VirtualDisplayInfo &vdinfo) {
    GError *raw_error = nullptr;
    vdinfo.mutter_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &raw_error);
    mutter_dbus::gerror_ptr dbus_error(raw_error);
    if (!vdinfo.mutter_bus) {
      BOOST_LOG(error) << "[VDISPLAY] Unable to connect to the session bus for " << vdinfo.name
                       << ": " << (dbus_error ? dbus_error->message : "unknown");
      return false;
    }

    // Capture the current physical primary BEFORE the virtual head exists, so teardown
    // can re-promote it and avoid leaving GNOME with a destroyed primary (headless lockout).
    vdinfo.saved_primary_connector = detect_mutter_primary_connector();
    if (!vdinfo.saved_primary_connector.empty()) {
      BOOST_LOG(info) << "[VDISPLAY] Saved pre-session primary connector '" << vdinfo.saved_primary_connector
                      << "' for " << vdinfo.name << " (restored on teardown).";
    } else {
      BOOST_LOG(warning) << "[VDISPLAY] Could not detect a physical primary connector for " << vdinfo.name
                         << "; teardown will rely on Mutter's fallback.";
    }

    if (!create_mutter_remote_desktop_session(vdinfo)) {
      destroy_mutter_virtual_stream(vdinfo);
      return false;
    }

    GVariantBuilder session_props;
    g_variant_builder_init(&session_props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(
      &session_props,
      "{sv}",
      "remote-desktop-session-id",
      g_variant_new_string(vdinfo.mutter_remote_desktop_session_id.c_str())
    );

    raw_error = nullptr;
    auto session_result = mutter_dbus::call_sync(
      vdinfo.mutter_bus,
      mutter_dbus::SCREENCAST_SERVICE,
      mutter_dbus::SCREENCAST_PATH,
      "org.gnome.Mutter.ScreenCast",
      "CreateSession",
      g_variant_new("(a{sv})", &session_props),
      G_VARIANT_TYPE("(o)"),
      mutter_dbus::QUICK_CALL_TIMEOUT_MS,
      &raw_error
    );
    dbus_error.reset(raw_error);
    if (!session_result) {
      BOOST_LOG(error) << "[VDISPLAY] Unable to create Mutter ScreenCast session for " << vdinfo.name
                       << ": " << (dbus_error ? dbus_error->message : "unknown");
      destroy_mutter_virtual_stream(vdinfo);
      return false;
    }

    const char *session_path_raw = nullptr;
    g_variant_get(session_result, "(&o)", &session_path_raw);
    vdinfo.mutter_session_path = session_path_raw ? session_path_raw : "";
    g_variant_unref(session_result);

    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "cursor-mode", g_variant_new_uint32(1));
    g_variant_builder_add(&props, "{sv}", "is-recording", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "is-platform", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "width", g_variant_new_uint32(vdinfo.width));
    g_variant_builder_add(&props, "{sv}", "height", g_variant_new_uint32(vdinfo.height));
    g_variant_builder_add(&props, "{sv}", "framerate", g_variant_new_uint32(std::max<uint32_t>(1, vdinfo.fps / 1000)));

    raw_error = nullptr;
    auto stream_result = mutter_dbus::call_sync(
      vdinfo.mutter_bus,
      mutter_dbus::SCREENCAST_SERVICE,
      vdinfo.mutter_session_path.c_str(),
      "org.gnome.Mutter.ScreenCast.Session",
      "RecordVirtual",
      g_variant_new("(a{sv})", &props),
      G_VARIANT_TYPE("(o)"),
      mutter_dbus::QUICK_CALL_TIMEOUT_MS,
      &raw_error
    );
    dbus_error.reset(raw_error);
    if (!stream_result) {
      BOOST_LOG(error) << "[VDISPLAY] Unable to create Mutter virtual ScreenCast stream for " << vdinfo.name
                       << ": " << (dbus_error ? dbus_error->message : "unknown");
      destroy_mutter_virtual_stream(vdinfo);
      return false;
    }

    const char *stream_path_raw = nullptr;
    g_variant_get(stream_result, "(&o)", &stream_path_raw);
    vdinfo.mutter_stream_path = stream_path_raw ? stream_path_raw : "";
    g_variant_unref(stream_result);

    mutter_node_wait_t node_wait;
    const auto signal_id = g_dbus_connection_signal_subscribe(
      vdinfo.mutter_bus,
      mutter_dbus::SCREENCAST_SERVICE,
      "org.gnome.Mutter.ScreenCast.Stream",
      "PipeWireStreamAdded",
      vdinfo.mutter_stream_path.c_str(),
      nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *parameters, gpointer user_data) {
        auto state = static_cast<mutter_node_wait_t *>(user_data);
        guint node = 0;
        g_variant_get(parameters, "(u)", &node);
        {
          std::lock_guard lock(state->mutex);
          state->node_id = node;
        }
        state->cv.notify_all();
      },
      &node_wait,
      nullptr
    );

    const bool started = !vdinfo.mutter_remote_desktop_session_path.empty() ?
      call_dbus_no_args(
        vdinfo,
        mutter_dbus::REMOTE_DESKTOP_SERVICE,
        vdinfo.mutter_remote_desktop_session_path,
        "org.gnome.Mutter.RemoteDesktop.Session",
        "Start"
      ) :
      call_dbus_no_args(vdinfo, mutter_dbus::SCREENCAST_SERVICE, vdinfo.mutter_session_path, "org.gnome.Mutter.ScreenCast.Session", "Start");
    if (!started) {
      if (signal_id) {
        g_dbus_connection_signal_unsubscribe(vdinfo.mutter_bus, signal_id);
      }
      destroy_mutter_virtual_stream(vdinfo);
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
      {
        std::lock_guard lock(node_wait.mutex);
        if (node_wait.node_id) {
          break;
        }
      }
      std::this_thread::sleep_for(20ms);
    }

    if (signal_id) {
      g_dbus_connection_signal_unsubscribe(vdinfo.mutter_bus, signal_id);
    }

    {
      std::lock_guard lock(node_wait.mutex);
      vdinfo.pipewire_node_id = node_wait.node_id;
    }

    if (!vdinfo.pipewire_node_id) {
      BOOST_LOG(error) << "[VDISPLAY] Timed out waiting for Mutter PipeWire node for " << vdinfo.name;
      destroy_mutter_virtual_stream(vdinfo);
      return false;
    }

    BOOST_LOG(info) << "[VDISPLAY] Created Mutter/PipeWire virtual display " << vdinfo.name
                    << " node=" << vdinfo.pipewire_node_id
                    << " mode=" << vdinfo.width << 'x' << vdinfo.height
                    << '@' << std::max<uint32_t>(1, vdinfo.fps / 1000) << "Hz";
    return true;
  }
#else
  static bool mutter_screencast_available() {
    BOOST_LOG(error) << "[VDISPLAY] Mutter/PipeWire virtual display backend is not compiled in.";
    return false;
  }
#endif

  static bool ensure_backend_available_locked(BACKEND backend) {
    switch (backend) {
      case BACKEND::MUTTER_PIPEWIRE:
        return mutter_screencast_available();

      case BACKEND::GAMESCOPE_PIPEWIRE: {
  #ifndef SUNSHINE_BUILD_PIPEWIRE
        BOOST_LOG(error) << "[VDISPLAY] Gamescope backend requires PipeWire support.";
        return false;
  #else
        auto binary = gamescope_binary();
        if (!gamescope_binary_available(binary)) {
          BOOST_LOG(error) << "[VDISPLAY] Gamescope backend requested, but Gamescope binary is not executable: " << binary;
          return false;
        }
        return true;
  #endif
      }

      case BACKEND::UNKNOWN:
      default:
        BOOST_LOG(error) << "[VDISPLAY] Unknown Linux virtual display backend requested.";
        return false;
    }
  }

  // ============================================================================
  // Utility Functions
  // ============================================================================

  static std::string generate_display_name(const uuid_util::uuid_t &guid) {
    return "VIRTUAL-" + guid.string().substr(0, 8);
  }

  // physical_power: "off" -> blank physical outputs (PowerSaveMode=3) after applying the layout;
  // "on" -> wake them (PowerSaveMode=0); "none" -> leave power state untouched. Only the
  // RecordVirtual path uses "off"/"on": on NVIDIA, dropping the physical monitor from the layout
  // is not enough to power down its connector (it keeps scanning out a frozen frame), so the
  // RecordVirtual isolate explicitly DPMS-offs it, and teardown wakes it back. Any other
  // caller must leave this at the "none" default.
  static bool apply_mutter_display_config(uint32_t width, uint32_t height, uint32_t refresh_hz, bool isolate, const std::string &primary_override = std::string(), bool restore_mode = false, const std::string &physical_power = std::string("none")) {
    if (!primary_override.empty() && !is_safe_connector_name(primary_override)) {
      BOOST_LOG(warning) << "[VDISPLAY] Refusing unsafe primary connector override.";
      return false;
    }
    const std::string helper_dir = apollo_helper_dir();
    if (helper_dir.empty()) {
      BOOST_LOG(warning) << "[VDISPLAY] XDG_RUNTIME_DIR unavailable; cannot run Mutter display helper.";
      return false;
    }
    const std::string script_path = helper_dir + "/apollo-mutter-displayconfig.py";

    std::ofstream script(script_path, std::ios::trunc);
    if (!script) {
      BOOST_LOG(warning) << "[VDISPLAY] Could not write Mutter display helper: " << strerror(errno);
      return false;
    }

    script << R"PY(#!/usr/bin/env python3
import sys
import time

from gi.repository import Gio, GLib

target_width = int(sys.argv[1])
target_height = int(sys.argv[2])
target_refresh = int(sys.argv[3])
isolate = sys.argv[4] == "1"
primary_override = sys.argv[5] if len(sys.argv) > 5 else ""
restore_mode = (sys.argv[6] == "1") if len(sys.argv) > 6 else False
power_action = sys.argv[7] if len(sys.argv) > 7 else "none"


def monitor_key(spec):
    return tuple(spec)


def is_apollo_monitor(monitor):
    spec, _modes, props = monitor
    connector, vendor, product, _serial = spec
    display_name = str(props.get("display-name", ""))
    haystack = " ".join([connector, vendor, product, display_name]).upper()
    return (
        vendor.upper() == "APL"
        or "APOLLO" in haystack
        or "VDISP" in haystack
        or vendor.upper() == "METAVENDOR"
        or connector.startswith("Meta-")
        or "VIRTUAL REMOTE MONITOR" in haystack
    )


def mode_prop(mode, name):
    props = mode[6]
    return bool(props.get(name, False))


def pick_mode(monitor, width=None, height=None, refresh=None):
    modes = monitor[1]
    if width and height:
        exact = [
            mode for mode in modes
            if mode[1] == width
            and mode[2] == height
            and (not refresh or abs(float(mode[3]) - float(refresh)) < 1.0)
        ]
        if exact:
            return exact[0]

        size_match = [mode for mode in modes if mode[1] == width and mode[2] == height]
        if size_match:
            return size_match[0]

    for mode in modes:
        if mode_prop(mode, "is-current"):
            return mode

    for mode in modes:
        if mode_prop(mode, "is-preferred"):
            return mode

    return modes[0] if modes else None


def get_state_for_name(name):
    return bus.call_sync(
        name,
        "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig",
        "GetCurrentState",
        None,
        None,
        Gio.DBusCallFlags.NO_AUTO_START,
        120,
        None,
    ).unpack()


bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

dbus_proxy = Gio.DBusProxy.new_sync(
    bus,
    Gio.DBusProxyFlags.NONE,
    None,
    "org.freedesktop.DBus",
    "/org/freedesktop/DBus",
    "org.freedesktop.DBus",
    None,
)


def bus_names_for_gnome_shell():
    names = ["org.gnome.Mutter.DisplayConfig"]
    try:
        all_names = dbus_proxy.call_sync(
            "ListNames",
            None,
            Gio.DBusCallFlags.NONE,
            1000,
            None,
        ).unpack()[0]
    except Exception:
        return names

    for name in all_names:
        if not name.startswith(":"):
            continue

        try:
            pid = dbus_proxy.call_sync(
                "GetConnectionUnixProcessID",
                GLib.Variant("(s)", (name,)),
                Gio.DBusCallFlags.NONE,
                300,
                None,
            ).unpack()[0]
            with open(f"/proc/{pid}/comm", "r", encoding="utf-8") as comm_file:
                if comm_file.read().strip() == "gnome-shell":
                    names.append(name)
        except Exception:
            continue

    return list(dict.fromkeys(names))


def find_target(candidate_state):
    # restore mode: promote a real physical connector, never a virtual head
    if restore_mode:
        if primary_override:
            for monitor in candidate_state[1]:
                if monitor[0][0] == primary_override and not is_apollo_monitor(monitor):
                    return monitor
        for monitor in candidate_state[1]:
            if not is_apollo_monitor(monitor):
                return monitor
        return None
    # normal mode: promote the Apollo/Meta virtual head
    for monitor in candidate_state[1]:
        if is_apollo_monitor(monitor):
            return monitor
    return None


state = None
target_monitor = None
proxy_name = None
candidate_names = bus_names_for_gnome_shell()
for _attempt in range(40):
    for candidate_name in candidate_names:
        try:
            candidate_state = get_state_for_name(candidate_name)
        except Exception:
            continue

        found = find_target(candidate_state)
        if found:
            proxy_name = candidate_name
            state = candidate_state
            target_monitor = found
            break

    if target_monitor:
        break

    time.sleep(0.1)

if not target_monitor:
    print("[VDISPLAY] target monitor not visible to Mutter (override=%r)" % primary_override, file=sys.stderr)
    sys.exit(2)

serial, monitors, logical_monitors, properties = state
monitor_by_spec = {monitor_key(monitor[0]): monitor for monitor in monitors}
primary_spec = target_monitor[0]
if restore_mode:
    primary_mode = pick_mode(target_monitor)
else:
    primary_mode = pick_mode(target_monitor, target_width, target_height, target_refresh)

if not primary_mode:
    print("[VDISPLAY] target monitor has no modes", file=sys.stderr)
    sys.exit(3)

layout_mode = int(properties.get("layout-mode", 1))
logical_config = [
    (
        0,
        0,
        float(primary_mode[4]),
        0,
        True,
        [(primary_spec[0], primary_mode[0], {})],
    )
]

next_x = int(primary_mode[1])
if restore_mode or not isolate:
    added = {monitor_key(primary_spec)}
    for logical in logical_monitors:
        scale = float(logical[2])
        transform = int(logical[3])
        for spec in logical[5]:
            key = monitor_key(spec)
            if key in added:
                continue

            monitor = monitor_by_spec.get(key)
            if not monitor:
                continue

            # in restore mode never re-add the virtual head (it is being destroyed)
            if restore_mode and is_apollo_monitor(monitor):
                continue

            mode = pick_mode(monitor)
            if not mode:
                continue

            logical_config.append(
                (
                    next_x,
                    0,
                    scale,
                    transform,
                    False,
                    [(spec[0], mode[0], {})],
                )
            )
            next_x += int(mode[1])
            added.add(key)

params = GLib.Variant(
    "(uua(iiduba(ssa{sv}))a{sv})",
    (
        int(serial),
        1,
        logical_config,
        {"layout-mode": GLib.Variant("u", layout_mode)},
    ),
)

bus.call_sync(
    proxy_name,
    "/org/gnome/Mutter/DisplayConfig",
    "org.gnome.Mutter.DisplayConfig",
    "ApplyMonitorsConfig",
    params,
    None,
    Gio.DBusCallFlags.NONE,
    3000,
    None,
)

print(
    f"[VDISPLAY] Applied Mutter monitor layout: primary {primary_spec[0]} "
    f"{primary_mode[1]}x{primary_mode[2]}@{float(primary_mode[3]):.3f}Hz, "
    f"isolate={isolate}, override={primary_override!r}, bus={proxy_name}"
)

# On NVIDIA, a monitor dropped from the layout is not actually powered down -- the connector
# keeps scanning out its last frame. Use Mutter's PowerSaveMode (DPMS) to truly blank the
# physical output while the RecordVirtual head is primary, and wake it again on restore.
if power_action in ("off", "on"):
    power_mode = 3 if power_action == "off" else 0
    try:
        bus.call_sync(
            proxy_name,
            "/org/gnome/Mutter/DisplayConfig",
            "org.freedesktop.DBus.Properties",
            "Set",
            GLib.Variant(
                "(ssv)",
                (
                    "org.gnome.Mutter.DisplayConfig",
                    "PowerSaveMode",
                    GLib.Variant("i", power_mode),
                ),
            ),
            None,
            Gio.DBusCallFlags.NONE,
            3000,
            None,
        )
        print(f"[VDISPLAY] Set Mutter PowerSaveMode={power_mode} ({power_action}) on {proxy_name}")
    except Exception as exc:
        print(f"[VDISPLAY] Setting PowerSaveMode={power_mode} failed: {exc}", file=sys.stderr)
)PY";
    script.close();

    if (!script) {
      BOOST_LOG(warning) << "[VDISPLAY] Could not finish writing Mutter display helper: " << strerror(errno);
      return false;
    }

    // Whitelist the power token so only a known literal can ever reach the shell.
    const char *power_token = "none";
    if (physical_power == "off") {
      power_token = "off";
    } else if (physical_power == "on") {
      power_token = "on";
    }

    std::ostringstream command;
    command << "timeout 8s python3 " << script_path << ' '
            << width << ' '
            << height << ' '
            << refresh_hz << ' '
            << (isolate ? 1 : 0) << " '"
            << primary_override << "' "
            << (restore_mode ? 1 : 0) << ' '
            << power_token;

    int result = std::system(command.str().c_str());
    if (result == 0) {
      BOOST_LOG(info) << "[VDISPLAY] Mutter display layout applied successfully.";
      return true;
    }

    BOOST_LOG(warning) << "[VDISPLAY] Mutter display layout helper failed with status " << result;
    return false;
  }

  // ============================================================================
  // Public API Implementation
  // ============================================================================

  DRIVER_STATUS openVDisplayDevice() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (driver_status == DRIVER_STATUS::OK) {
      return driver_status;
    }

    BOOST_LOG(info) << "[VDISPLAY] Initializing Linux virtual display driver...";

    selected_backend = configured_backend();
    BOOST_LOG(info) << "[VDISPLAY] Requested Linux virtual display backend: " << linuxVirtualDisplayBackendName(selected_backend);

    if (!ensure_backend_available_locked(selected_backend)) {
      driver_status = DRIVER_STATUS::NOT_SUPPORTED;
      return driver_status;
    }

    BOOST_LOG(info) << "[VDISPLAY] " << linuxVirtualDisplayBackendName(selected_backend) << " backend available.";
    driver_status = DRIVER_STATUS::OK;
    BOOST_LOG(info) << "[VDISPLAY] Linux virtual display driver initialized successfully.";

    return driver_status;
  }

  void closeVDisplayDevice() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    BOOST_LOG(info) << "[VDISPLAY] Closing Linux virtual display driver...";

    // Stop watchdog thread
    watchdog_running = false;
    if (watchdog_thread.joinable()) {
      watchdog_thread.join();
    }

    // Clean up all virtual displays
    for (auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active) {
#ifdef SUNSHINE_BUILD_PIPEWIRE
        if (vdinfo.backend == BACKEND::MUTTER_PIPEWIRE) {
          destroy_mutter_virtual_stream(vdinfo);
        }
#endif
        if (vdinfo.gamescope) {
          vdinfo.gamescope->stop();
        }
        if (vdinfo.drm_fd >= 0) {
          ::close(vdinfo.drm_fd);
        }
      }
    }
    virtual_displays.clear();

    driver_status = DRIVER_STATUS::UNKNOWN;
    BOOST_LOG(info) << "[VDISPLAY] Linux virtual display driver closed.";
  }

  bool startPingThread(std::function<void()> failCb) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (watchdog_running) {
      return true;
    }

    watchdog_running = true;

    watchdog_thread = std::thread([failCb = std::move(failCb)]() {
      BOOST_LOG(debug) << "[VDISPLAY] Watchdog thread started.";

      while (watchdog_running) {
        std::this_thread::sleep_for(5s);

        if (!watchdog_running) {
          break;
        }

        std::lock_guard<std::mutex> lock(vdisplay_mutex);

        // Watchdog loop retained for future virtual-display liveness checks.
        // The Mutter/Gamescope backends do not currently expose a per-device
        // health probe, so there is no per-display work to perform here.
        (void) failCb;
      }

      BOOST_LOG(debug) << "[VDISPLAY] Watchdog thread stopped.";
    });

    return true;
  }

  bool setRenderAdapterByName(const std::string &adapterName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (adapterName.empty()) {
      BOOST_LOG(debug) << "[VDISPLAY] No specific adapter requested.";
      return true;
    }

    BOOST_LOG(info) << "[VDISPLAY] Adapter hint: " << adapterName;
    // On Linux, we don't need to select a specific render adapter here.
    return true;
  }

  std::string createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const uuid_util::uuid_t &guid,
    std::optional<BACKEND> backend_override
  ) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    if (driver_status != DRIVER_STATUS::OK) {
      BOOST_LOG(error) << "[VDISPLAY] Driver not initialized.";
      return "";
    }

    const auto requested_backend = backend_override.value_or(selected_backend);
    if (!ensure_backend_available_locked(requested_backend)) {
      BOOST_LOG(error) << "[VDISPLAY] " << linuxVirtualDisplayBackendName(requested_backend)
                       << " virtual display backend is not available for this session.";
      return "";
    }

    std::string guid_str = guid.string();
    std::string display_name = generate_display_name(guid);

    uint32_t fps_hz = fps;
    if (fps_hz == 0) {
      fps_hz = 60;
    } else if (fps_hz >= 1000) {
      fps_hz /= 1000;
    }

    BOOST_LOG(info) << "[VDISPLAY] Creating virtual display: " << display_name
                    << " (W: " << width << ", H: " << height << ", FPS: " << fps_hz << ")";
    BOOST_LOG(info) << "[VDISPLAY] Client: " << s_client_name << " (" << s_client_uid << ")";

    VirtualDisplayInfo vdinfo;
    vdinfo.name = display_name;
    vdinfo.guid_str = guid_str;
    vdinfo.width = width;
    vdinfo.height = height;
    vdinfo.fps = fps_hz * 1000;
    vdinfo.backend = requested_backend;
    vdinfo.drm_fd = -1;
    vdinfo.active = true;
    vdinfo.gamescope_cursor.x = static_cast<double>(width) / 2.0;
    vdinfo.gamescope_cursor.y = static_cast<double>(height) / 2.0;
    vdinfo.gamescope_cursor.width = width;
    vdinfo.gamescope_cursor.height = height;

    if (requested_backend == BACKEND::MUTTER_PIPEWIRE) {
#ifdef SUNSHINE_BUILD_PIPEWIRE
      if (!create_mutter_virtual_stream(vdinfo)) {
        BOOST_LOG(error) << "[VDISPLAY] Mutter/PipeWire virtual display creation failed for " << display_name;
        return "";
      }
#else
      BOOST_LOG(error) << "[VDISPLAY] Mutter/PipeWire backend is not compiled in.";
      return "";
#endif
    } else if (requested_backend == BACKEND::GAMESCOPE_PIPEWIRE) {
      vdinfo.gamescope = std::make_shared<gamescope_session_t>();
      if (!vdinfo.gamescope->start(display_name, width, height, fps_hz)) {
        BOOST_LOG(error) << "[VDISPLAY] Gamescope/PipeWire virtual display creation failed for " << display_name;
        return "";
      }
    }

    const auto backend = vdinfo.backend;
    virtual_displays[guid_str] = std::move(vdinfo);

    BOOST_LOG(info) << "[VDISPLAY] Virtual display created successfully: " << display_name;
    BOOST_LOG(info) << "[VDISPLAY] Mode: " << linuxVirtualDisplayBackendName(backend);

    return display_name;
  }

  bool removeVirtualDisplay(const uuid_util::uuid_t &guid) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    std::string guid_str = guid.string();

    auto it = virtual_displays.find(guid_str);
    if (it == virtual_displays.end()) {
      BOOST_LOG(warning) << "[VDISPLAY] Virtual display not found: " << guid_str;
      return false;
    }

    auto &vdinfo = it->second;
    BOOST_LOG(info) << "[VDISPLAY] Removing virtual display: " << vdinfo.name;

#ifdef SUNSHINE_BUILD_PIPEWIRE
    if (vdinfo.backend == BACKEND::MUTTER_PIPEWIRE) {
      destroy_mutter_virtual_stream(vdinfo);
    }
#endif
    if (vdinfo.gamescope) {
      vdinfo.gamescope->stop();
    }

    if (vdinfo.drm_fd >= 0) {
      ::close(vdinfo.drm_fd);
    }

    virtual_displays.erase(it);

    BOOST_LOG(info) << "[VDISPLAY] Virtual display removed successfully.";
    return true;
  }

  static int change_display_settings(const char *deviceName, int width, int height, int refresh_rate, bool isolate) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    // Convert from mHz to Hz
    int refresh_hz = refresh_rate;
    if (refresh_hz == 0) {
      refresh_hz = 60;
    } else if (refresh_hz >= 1000) {
      refresh_hz /= 1000;
    }

    BOOST_LOG(info) << "[VDISPLAY] Changing display settings for " << deviceName
                    << " to " << width << "x" << height << "@" << refresh_hz << "Hz";

    // Find the virtual display
    for (auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.name == deviceName) {
        bool mode_unchanged = vdinfo.width == static_cast<uint32_t>(width) &&
                              vdinfo.height == static_cast<uint32_t>(height) &&
                              vdinfo.fps == static_cast<uint32_t>(refresh_hz * 1000);

        vdinfo.width = width;
        vdinfo.height = height;
        vdinfo.fps = refresh_hz * 1000;

        if (vdinfo.backend == BACKEND::MUTTER_PIPEWIRE) {
          if (!mode_unchanged) {
            BOOST_LOG(warning) << "[VDISPLAY] Mutter/PipeWire virtual display mode changes require stream recreation; "
                               << vdinfo.name << " will keep its active PipeWire mode until the next session.";
          }
          BOOST_LOG(info) << "[VDISPLAY] Mutter/PipeWire virtual display mode recorded; desktop layout is applied by the capture stream after the first frame.";
          return 0;
        }

        if (vdinfo.backend == BACKEND::GAMESCOPE_PIPEWIRE) {
          if (!mode_unchanged) {
            BOOST_LOG(warning) << "[VDISPLAY] Gamescope virtual display mode changes require session recreation; "
                               << vdinfo.name << " will keep its active PipeWire mode until the next session.";
          }
          BOOST_LOG(info) << "[VDISPLAY] Gamescope virtual display mode recorded successfully.";
          return 0;
        }

        BOOST_LOG(info) << "[VDISPLAY] Display settings updated successfully.";
        return 0;
      }
    }

    BOOST_LOG(debug) << "[VDISPLAY] Display not found: " << deviceName;
    return 0;
  }

  int changeDisplaySettings(const char *deviceName, int width, int height, int refresh_rate) {
    return change_display_settings(deviceName, width, height, refresh_rate, false);
  }

  int changeDisplaySettings2(const char *deviceName, int width, int height, int refresh_rate, bool bApplyIsolated) {
    if (bApplyIsolated) {
      BOOST_LOG(debug) << "[VDISPLAY] Applying isolated virtual display layout.";
    }
    return change_display_settings(deviceName, width, height, refresh_rate, bApplyIsolated);
  }

  std::string getPrimaryDisplay() {
    // Return first connected physical display
    try {
      for (const auto &entry : fs::directory_iterator("/dev/dri")) {
        const auto &path = entry.path();
        std::string filename = path.filename().string();
        if (filename.find("card") == 0 && filename.find("render") == std::string::npos) {
          int fd = ::open(path.c_str(), O_RDWR);
          if (fd >= 0) {
            drmModeRes *res = drmModeGetResources(fd);
            if (res) {
              for (int i = 0; i < res->count_connectors; i++) {
                drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
                if (conn && conn->connection == DRM_MODE_CONNECTED) {
                  std::string name = "HDMI-A-" + std::to_string(conn->connector_type_id);
                  drmModeFreeConnector(conn);
                  drmModeFreeResources(res);
                  ::close(fd);
                  return name;
                }
                if (conn) drmModeFreeConnector(conn);
              }
              drmModeFreeResources(res);
            }
            ::close(fd);
          }
        }
      }
    } catch (...) {}
    return "";
  }

  bool setPrimaryDisplay(const char *primaryDeviceName) {
    BOOST_LOG(debug) << "[VDISPLAY] setPrimaryDisplay is a no-op on Linux.";
    return true;
  }

  bool getDisplayHDRByName(const char *displayName) {
    BOOST_LOG(debug) << "[VDISPLAY] HDR check for: " << displayName;
    // HDR is not supported for Linux virtual displays currently.
    return false;
  }

  bool setDisplayHDRByName(const char *displayName, bool enableAdvancedColor) {
    BOOST_LOG(debug) << "[VDISPLAY] HDR setting not supported on Linux.";
    return false;
  }

  std::vector<std::string> matchDisplay(const std::string &sMatch) {
    std::vector<std::string> matches;

    std::lock_guard<std::mutex> lock(vdisplay_mutex);

    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name.find(sMatch) != std::string::npos) {
        matches.push_back(vdinfo.name);
      }
    }

    return matches;
  }

  bool isVirtualDisplay(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name == displayName) {
        return true;
      }
    }
    return false;
  }

  BACKEND virtualDisplayBackend(const std::string &displayName) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name == displayName) {
        return vdinfo.backend;
      }
    }
    return BACKEND::UNKNOWN;
  }

  bool getVirtualDisplayMode(const std::string &displayName, uint32_t &width, uint32_t &height, uint32_t &fps) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name == displayName) {
        width = vdinfo.width;
        height = vdinfo.height;
        fps = vdinfo.fps;
        return true;
      }
    }
    return false;
  }

  bool getMutterPipeWireNodeId(const std::string &displayName, uint32_t &node_id) {
#ifdef SUNSHINE_BUILD_PIPEWIRE
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name == displayName && vdinfo.backend == BACKEND::MUTTER_PIPEWIRE && vdinfo.pipewire_node_id) {
        node_id = vdinfo.pipewire_node_id;
        return true;
      }
    }
#else
    (void) displayName;
    (void) node_id;
#endif
    return false;
  }

  // Make the Mutter RecordVirtual head (Meta-0) the sole primary so the real desktop
  // renders onto it. Must be called AFTER the capture consumer is pulling frames (the
  // head only exists once a consumer is engaged). Snapshots mode under the lock, then
  // runs the (subprocess) helper without holding it.
  bool applyMutterDisplayLayout(const std::string &displayName, bool isolate) {
    uint32_t width = 0, height = 0, fps = 0;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(vdisplay_mutex);
      for (const auto &[guid, vdinfo] : virtual_displays) {
        if (vdinfo.active && vdinfo.name == displayName && vdinfo.backend == BACKEND::MUTTER_PIPEWIRE) {
          width = vdinfo.width;
          height = vdinfo.height;
          fps = vdinfo.fps;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      BOOST_LOG(warning) << "[VDISPLAY] applyMutterDisplayLayout: no active Mutter/PipeWire display named " << displayName;
      return false;
    }
    const uint32_t refresh_hz = std::max<uint32_t>(1, fps / 1000);
    BOOST_LOG(info) << "[VDISPLAY] Applying Mutter/PipeWire desktop layout for " << displayName
                    << " (" << width << "x" << height << "@" << refresh_hz << "Hz, isolate=" << isolate << ").";
    // When isolating onto Meta-0, also DPMS-off the physical output: on NVIDIA, removing it from
    // the layout leaves the panel lit on a frozen frame. "none" when not isolating.
    if (!apply_mutter_display_config(width, height, refresh_hz, isolate, std::string(), false, isolate ? "off" : "none")) {
      BOOST_LOG(warning) << "[VDISPLAY] applyMutterDisplayLayout failed for " << displayName;
      return false;
    }
    std::this_thread::sleep_for(750ms);
    return true;
  }

  // Re-promote the saved physical connector to primary (dropping the virtual head) while
  // the stream still exists, so destroying Meta-0 does not leave GNOME headless.
  bool restoreMutterPhysicalPrimary(const std::string &displayName) {
    std::string primary;
    bool is_mutter = false;
    {
      std::lock_guard<std::mutex> lock(vdisplay_mutex);
      for (const auto &[guid, vdinfo] : virtual_displays) {
        if (vdinfo.name == displayName && vdinfo.backend == BACKEND::MUTTER_PIPEWIRE) {
          is_mutter = true;
          primary = vdinfo.saved_primary_connector;
          break;
        }
      }
    }
    if (!is_mutter) {
      return false;  // not a Mutter/PipeWire display; nothing to restore
    }
    if (primary.empty()) {
      BOOST_LOG(warning) << "[VDISPLAY] restoreMutterPhysicalPrimary: no saved primary for " << displayName
                         << "; attempting to promote any available physical monitor.";
    } else {
      BOOST_LOG(info) << "[VDISPLAY] Restoring physical primary monitor '" << primary
                      << "' before tearing down " << displayName << '.';
    }
    // restore_mode=true so an empty/stale saved connector still falls back to ANY physical
    // monitor rather than no-op'ing (which would leave the virtual head primary on teardown).
    // physical_power="on" wakes the panel we DPMS-off'd during isolate (re-adding it to the
    // layout alone may leave it in power-save on NVIDIA).
    return apply_mutter_display_config(0, 0, 0, false, primary, /*restore_mode=*/true, /*physical_power=*/"on");
  }

  bool getGamescopePipeWireNodeId(const std::string &displayName, uint32_t &node_id) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name == displayName && vdinfo.backend == BACKEND::GAMESCOPE_PIPEWIRE && vdinfo.gamescope) {
        node_id = vdinfo.gamescope->node_id();
        return node_id != 0;
      }
    }
    return false;
  }

  bool getGamescopeLaunchEnvironment(const std::string &displayName, gamescope_launch_environment_t &environment) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name == displayName && vdinfo.backend == BACKEND::GAMESCOPE_PIPEWIRE && vdinfo.gamescope) {
        environment = vdinfo.gamescope->launch_environment();
        return !environment.wayland_display.empty();
      }
    }
    return false;
  }

  bool getGamescopeCursorState(const std::string &displayName, gamescope_cursor_state_t &state) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name == displayName && vdinfo.backend == BACKEND::GAMESCOPE_PIPEWIRE) {
        if (!vdinfo.gamescope_cursor_overlay) {
          return false;
        }
        state = vdinfo.gamescope_cursor;
        return true;
      }
    }
    return false;
  }

  bool setGamescopeCursorOverlay(const std::string &displayName, bool enabled) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.name == displayName && vdinfo.backend == BACKEND::GAMESCOPE_PIPEWIRE) {
        vdinfo.gamescope_cursor_overlay = enabled;
        if (!enabled) {
          vdinfo.gamescope_cursor.visible = false;
        }
        return true;
      }
    }
    return false;
  }

  struct gamescope_input_target_t {
    std::string guid;
    std::shared_ptr<gamescope_ei_client_t> input;
  };

  static std::optional<gamescope_input_target_t> active_gamescope_input_target() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.backend == BACKEND::GAMESCOPE_PIPEWIRE && vdinfo.gamescope) {
        return gamescope_input_target_t {
          guid,
          vdinfo.gamescope->input()
        };
      }
    }
    return std::nullopt;
  }

  static void update_gamescope_cursor_absolute(const std::string &guid, double x, double y) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    auto it = virtual_displays.find(guid);
    if (it == virtual_displays.end() || !it->second.active || it->second.backend != BACKEND::GAMESCOPE_PIPEWIRE) {
      return;
    }

    auto &cursor = it->second.gamescope_cursor;
    cursor.width = it->second.width;
    cursor.height = it->second.height;
    cursor.x = std::clamp(x, 0.0, cursor.width > 0 ? static_cast<double>(cursor.width - 1) : 0.0);
    cursor.y = std::clamp(y, 0.0, cursor.height > 0 ? static_cast<double>(cursor.height - 1) : 0.0);
    cursor.visible = true;
    ++cursor.serial;
  }

  static void update_gamescope_cursor_relative(const std::string &guid, double dx, double dy) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    auto it = virtual_displays.find(guid);
    if (it == virtual_displays.end() || !it->second.active || it->second.backend != BACKEND::GAMESCOPE_PIPEWIRE) {
      return;
    }

    auto &cursor = it->second.gamescope_cursor;
    cursor.width = it->second.width;
    cursor.height = it->second.height;
    if (!cursor.visible) {
      cursor.x = static_cast<double>(cursor.width) / 2.0;
      cursor.y = static_cast<double>(cursor.height) / 2.0;
    }
    cursor.x = std::clamp(cursor.x + dx, 0.0, cursor.width > 0 ? static_cast<double>(cursor.width - 1) : 0.0);
    cursor.y = std::clamp(cursor.y + dy, 0.0, cursor.height > 0 ? static_cast<double>(cursor.height - 1) : 0.0);
    cursor.visible = true;
    ++cursor.serial;
  }

  static std::optional<int> pointer_button_to_evdev(int button) {
    switch (button) {
      case 0x01:
        return BTN_LEFT;
      case 0x02:
        return BTN_MIDDLE;
      case 0x03:
        return BTN_RIGHT;
      case 0x04:
        return BTN_SIDE;
      case 0x05:
        return BTN_EXTRA;
      default:
        return std::nullopt;
    }
  }

  static std::shared_ptr<gamescope_ei_client_t> active_gamescope_input() {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (vdinfo.active && vdinfo.backend == BACKEND::GAMESCOPE_PIPEWIRE && vdinfo.gamescope) {
        return vdinfo.gamescope->input();
      }
    }
    return nullptr;
  }

#ifdef SUNSHINE_BUILD_PIPEWIRE
  struct mutter_remote_desktop_target_t {
    GDBusConnection *bus {};
    std::string session_path;
    std::string stream_path;
    uint32_t width {};
    uint32_t height {};
  };

  static bool get_active_mutter_remote_desktop_target(mutter_remote_desktop_target_t &target, bool require_stream) {
    std::lock_guard<std::mutex> lock(vdisplay_mutex);
    for (const auto &[guid, vdinfo] : virtual_displays) {
      if (!vdinfo.active ||
          vdinfo.backend != BACKEND::MUTTER_PIPEWIRE ||
          !vdinfo.mutter_bus ||
          vdinfo.mutter_remote_desktop_session_path.empty()) {
        continue;
      }
      if (require_stream && vdinfo.mutter_stream_path.empty()) {
        continue;
      }

      target.bus = vdinfo.mutter_bus;
      g_object_ref(target.bus);
      target.session_path = vdinfo.mutter_remote_desktop_session_path;
      target.stream_path = vdinfo.mutter_stream_path;
      target.width = vdinfo.width;
      target.height = vdinfo.height;
      return true;
    }

    return false;
  }

  static bool queue_mutter_remote_desktop_event(mutter_remote_desktop_target_t &target, const char *method, GVariant *parameters) {
    if (!target.bus) {
      return false;
    }

    g_dbus_connection_call(
      target.bus,
      mutter_dbus::REMOTE_DESKTOP_SERVICE,
      target.session_path.c_str(),
      "org.gnome.Mutter.RemoteDesktop.Session",
      method,
      parameters,
      nullptr,
      G_DBUS_CALL_FLAGS_NONE,
      1000,
      nullptr,
      nullptr,
      nullptr
    );
    g_object_unref(target.bus);
    target.bus = nullptr;
    return true;
  }
#endif

  bool notifyMutterPointerMotionRelative(double dx, double dy) {
#ifdef SUNSHINE_BUILD_PIPEWIRE
    mutter_remote_desktop_target_t target;
    if (!get_active_mutter_remote_desktop_target(target, false)) {
      return false;
    }

    return queue_mutter_remote_desktop_event(
      target,
      "NotifyPointerMotionRelative",
      g_variant_new("(dd)", dx, dy)
    );
#else
    (void) dx;
    (void) dy;
    return false;
#endif
  }

  bool notifyMutterPointerMotionAbsolute(double x, double y) {
#ifdef SUNSHINE_BUILD_PIPEWIRE
    GDBusConnection *bus {};
    std::string session_path;
    std::string stream_path;
    double clamped_x {};
    double clamped_y {};
    {
      std::lock_guard<std::mutex> lock(vdisplay_mutex);
      for (const auto &[guid, vdinfo] : virtual_displays) {
        if (!vdinfo.active ||
            vdinfo.backend != BACKEND::MUTTER_PIPEWIRE ||
            !vdinfo.mutter_bus ||
            vdinfo.mutter_remote_desktop_session_path.empty() ||
            vdinfo.mutter_stream_path.empty()) {
          continue;
        }

        bus = vdinfo.mutter_bus;
        g_object_ref(bus);
        session_path = vdinfo.mutter_remote_desktop_session_path;
        stream_path = vdinfo.mutter_stream_path;
        clamped_x = std::clamp(x, 0.0, static_cast<double>(vdinfo.width));
        clamped_y = std::clamp(y, 0.0, static_cast<double>(vdinfo.height));
        break;
      }
    }

    if (!bus) {
      return false;
    }

    mutter_remote_desktop_target_t target;
    target.bus = bus;
    target.session_path = session_path;
    return queue_mutter_remote_desktop_event(
      target,
      "NotifyPointerMotionAbsolute",
      g_variant_new("(sdd)", stream_path.c_str(), clamped_x, clamped_y)
    );
#else
    (void) x;
    (void) y;
    return false;
#endif
  }

  bool notifyMutterPointerButton(int button, bool release) {
#ifdef SUNSHINE_BUILD_PIPEWIRE
    // Mutter's NotifyPointerButton expects a Linux evdev button code (BTN_LEFT, BTN_RIGHT, ...),
    // not Sunshine's 1/2/3 button index. Translate first; an unmapped button falls through to
    // the uinput path rather than injecting a bogus code.
    auto code = pointer_button_to_evdev(button);
    if (!code) {
      return false;
    }

    mutter_remote_desktop_target_t target;
    if (!get_active_mutter_remote_desktop_target(target, false)) {
      return false;
    }

    return queue_mutter_remote_desktop_event(
      target,
      "NotifyPointerButton",
      g_variant_new("(ib)", *code, !release)
    );
#else
    (void) button;
    (void) release;
    return false;
#endif
  }

  bool notifyMutterPointerAxis(double dx, double dy) {
#ifdef SUNSHINE_BUILD_PIPEWIRE
    constexpr uint32_t source_wheel = 2;

    mutter_remote_desktop_target_t target;
    if (!get_active_mutter_remote_desktop_target(target, false)) {
      return false;
    }

    return queue_mutter_remote_desktop_event(
      target,
      "NotifyPointerAxis",
      g_variant_new("(ddu)", dx, dy, source_wheel)
    );
#else
    (void) dx;
    (void) dy;
    return false;
#endif
  }

  bool notifyMutterKeyboardKeycode(uint32_t evdev_keycode, bool release) {
#ifdef SUNSHINE_BUILD_PIPEWIRE
    mutter_remote_desktop_target_t target;
    if (!get_active_mutter_remote_desktop_target(target, false)) {
      return false;
    }

    return queue_mutter_remote_desktop_event(
      target,
      "NotifyKeyboardKeycode",
      g_variant_new("(ub)", evdev_keycode, !release)
    );
#else
    (void) evdev_keycode;
    (void) release;
    return false;
#endif
  }

  bool notifyGamescopePointerMotionRelative(double dx, double dy) {
    auto target = active_gamescope_input_target();
    if (!target || !target->input || !target->input->pointer_motion_relative(dx, dy)) {
      return false;
    }
    update_gamescope_cursor_relative(target->guid, dx, dy);
    return true;
  }

  bool notifyGamescopePointerMotionAbsolute(double x, double y) {
    auto target = active_gamescope_input_target();
    if (!target || !target->input || !target->input->pointer_motion_absolute(x, y)) {
      return false;
    }
    update_gamescope_cursor_absolute(target->guid, x, y);
    return true;
  }

  bool notifyGamescopePointerButton(int button, bool release) {
    auto input = active_gamescope_input();
    auto mapped_button = pointer_button_to_evdev(button);
    return input && mapped_button && input->pointer_button(*mapped_button, release);
  }

  bool notifyGamescopePointerAxis(double dx, double dy) {
    auto input = active_gamescope_input();
    return input && input->pointer_axis(dx, dy);
  }

  bool notifyGamescopeKeyboardKey(uint16_t modcode, bool release) {
    auto input = active_gamescope_input();
    return input && input->keyboard_key(modcode, release);
  }

}  // namespace VDISPLAY

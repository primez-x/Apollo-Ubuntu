/**
 * @file src/platform/linux/cuda.h
 * @brief Definitions for CUDA implementation.
 */
#pragma once

#if defined(SUNSHINE_BUILD_CUDA)
  // standard includes
  #include <cstdint>
  #include <memory>
  #include <optional>
  #include <string>
  #include <vector>

  // local includes
  #include "src/video_colorspace.h"

namespace platf {
  struct avcodec_encode_device_t;
  struct img_t;
}  // namespace platf

namespace cuda {

  namespace nvfbc {
    std::vector<std::string> display_names();
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram);

  /**
   * @brief Create a GL->CUDA encoding device for consuming captured dmabufs.
   * @param in_width Width of captured frames.
   * @param in_height Height of captured frames.
   * @param offset_x Offset of content in captured frame.
   * @param offset_y Offset of content in captured frame.
   * @return FFmpeg encoding device context.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_encode_device(int width, int height, int offset_x, int offset_y);
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_encode_device(int width, int height, int offset_x, int offset_y, int drm_fd);

  /**
   * @brief Query the DRM format modifiers the GL->CUDA importer can accept.
   *
   * Builds a throwaway GBM/EGL display on the encode GPU's DRM node and queries
   * egl::query_dmabuf_modifiers for each supplied DRM fourcc. When @p drm_fd is
   * >= 0 the probe display is built on a dup() of that fd (the resolved encode
   * GPU); when -1 it falls back to the legacy CUDA device 0 node
   * (open_drm_fd_for_cuda_device(0)). The union of the importable
   * (non-external-only) modifiers is returned. The probe display is torn down
   * before returning. Returns empty on any failure.
   *
   * @param fourccs DRM fourccs (e.g. DRM_FORMAT_XRGB8888) to query.
   * @param drm_fd DRM render-node fd of the encode GPU, or -1 to use CUDA device 0.
   * @return The union of importable modifiers across the fourccs; empty on failure.
   */
  std::vector<std::uint64_t> query_importable_modifiers(const std::vector<std::uint32_t> &fourccs, int drm_fd = -1);

  int init();
}  // namespace cuda

typedef struct cudaArray *cudaArray_t;

  #if !defined(__CUDACC__)
typedef struct CUstream_st *cudaStream_t;
typedef unsigned long long cudaTextureObject_t;
  #else /* defined(__CUDACC__) */
typedef __location__(device_builtin) struct CUstream_st *cudaStream_t;
typedef __location__(device_builtin) unsigned long long cudaTextureObject_t;
  #endif /* !defined(__CUDACC__) */

namespace cuda {

  class freeCudaPtr_t {
  public:
    void operator()(void *ptr);
  };

  class freeCudaStream_t {
  public:
    void operator()(cudaStream_t ptr);
  };

  using ptr_t = std::unique_ptr<void, freeCudaPtr_t>;
  using stream_t = std::unique_ptr<CUstream_st, freeCudaStream_t>;

  stream_t make_stream(int flags = 0);

  struct viewport_t {
    int width, height;
    int offsetX, offsetY;
  };

  class tex_t {
  public:
    static std::optional<tex_t> make(int height, int pitch);

    tex_t();
    tex_t(tex_t &&);

    tex_t &operator=(tex_t &&other);

    ~tex_t();

    int copy(std::uint8_t *src, int height, int pitch);

    cudaArray_t array;

    struct texture {
      cudaTextureObject_t point;
      cudaTextureObject_t linear;
    } texture;
  };

  class sws_t {
  public:
    sws_t() = default;
    sws_t(int in_width, int in_height, int out_width, int out_height, int pitch, int threadsPerBlock, ptr_t &&color_matrix);

    /**
     * in_width, in_height -- The width and height of the captured image in pixels
     * out_width, out_height -- the width and height of the NV12 image in pixels
     *
     * pitch -- The size of a single row of pixels in bytes
     */
    static std::optional<sws_t> make(int in_width, int in_height, int out_width, int out_height, int pitch);

    // Converts loaded image into a CUDevicePtr
    int convert(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream);
    int convert(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream, const viewport_t &viewport);

    void apply_colorspace(const video::sunshine_colorspace_t &colorspace);

    int load_ram(platf::img_t &img, cudaArray_t array);

    ptr_t color_matrix;

    int threadsPerBlock;

    viewport_t viewport;

    float scale;
  };
}  // namespace cuda

#endif

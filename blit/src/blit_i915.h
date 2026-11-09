#ifndef BLIT_I915_H
#define BLIT_I915_H

#include <stdint.h>

int  blit_i915_init(int drm_fd);
void blit_i915_shutdown(void);
int  blit_i915_available(void);

int  blit_i915_copy(uint32_t src_handle, uint32_t src_pitch,
                    uint32_t dst_handle, uint32_t dst_pitch,
                    uint32_t width, uint32_t height,
                    uint32_t *out_batch_handle);

int  blit_i915_wait(uint32_t batch_handle, int64_t timeout_ns);
void blit_i915_close_batch(uint32_t batch_handle);

#endif

/* Register a (cpu_ptr, gem_handle, pitch) triple so the blit library
 * can map blit_desc_t src/dst pointers back to i915 GEM handles. */
void blit_i915_register_ptr(const void *ptr, uint32_t gem_handle,
                            uint32_t pitch);
void blit_i915_unregister_ptr(const void *ptr);

/* Open the render node from CHIPMEM_RENDER_NODE if set, otherwise
 * /dev/dri/renderD128. Returns 0 on success. */
int blit_i915_init_from_env(void);

/* Map src/dst pointers to GEM handles and submit the copy. */
int blit_i915_copy_from_ptr(const void *src, void *dst,
                            uint32_t width, uint32_t height, uint8_t bpp,
                            uint32_t src_stride, uint32_t dst_stride,
                            uint32_t *out_src_handle,
                            uint32_t *out_dst_handle,
                            uint32_t *out_batch);

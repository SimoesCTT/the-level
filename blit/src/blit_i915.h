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

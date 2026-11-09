#ifndef CHIPMEM_H
#define CHIPMEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * chipmem — a Linux-native coherent CPU/accelerator memory abstraction.
 *
 * Thesis: the Amiga's MEMF_CHIP was a single allocation flag with an
 * implicit, well-understood coherency model. Linux has the plumbing
 * (DMA-BUF, mmap, begin/end_cpu_access) but not the simplicity. This
 * header restores the simplicity without hiding the contract.
 *
 * The backend is determined at runtime and reported by
 * chipmem_backend(). Callers must check it before assuming sync is
 * cheap or free.
 */

typedef enum {
    /*
     * Hardware-snooped coherency. chipmem_sync() is a no-op by kernel
     * contract. Rare. Not the case on i915 or amdgpu today.
     */
    CHIPMEM_BACKEND_DMABUF_COHERENT = 0,

    /*
     * DMA-BUF with mandatory begin/end_cpu_access bracket.
     * chipmem_sync() issues DMA_BUF_IOCTL_SYNC. This is the i915 case.
     */
    CHIPMEM_BACKEND_DMABUF_SYNC_IOCTL = 1,

    /*
     * No zero-copy path. chipmem_alloc() still returns a CPU pointer,
     * but chipmem_sync() flushes a hidden staging buffer. Cost is
     * reported by chipmem_backend_cost().
     */
    CHIPMEM_BACKEND_COPY_FALLBACK = 2
} chipmem_backend_t;

typedef enum {
    CHIPMEM_ACCESS_CPU_READ    = 0x1,
    CHIPMEM_ACCESS_CPU_WRITE   = 0x2,
    CHIPMEM_ACCESS_ACCEL_READ  = 0x4,
    CHIPMEM_ACCESS_ACCEL_WRITE = 0x8
} chipmem_access_t;

typedef struct chipmem_buffer chipmem_buffer_t;

/*
 * Allocate a buffer visible to both CPU and accelerator.
 *
 * Returns NULL on failure. On success, *out_backend is set to the
 * backend actually used.
 *
 * The CPU pointer is valid immediately. The accelerator must NOT access
 * the buffer until chipmem_sync() is called with the appropriate flags.
 */
/*
 * Use an existing DRM fd for subsequent chipmem_alloc calls. This
 * allows chipmem buffers to share a GEM handle namespace with a
 * backend that submits batches directly (blit_i915). If not called,
 * chipmem_alloc opens its own render node.
 */
void chipmem_set_fd(int fd);

chipmem_buffer_t *chipmem_alloc(size_t size,
                                chipmem_backend_t *out_backend);

/*
 * Get a CPU-accessible pointer. Valid for the lifetime of the buffer.
 * Writes through this pointer are NOT visible to the accelerator until
 * chipmem_sync(CHIPMEM_ACCESS_CPU_WRITE) is called. Conversely,
 * accelerator writes are NOT visible to the CPU until
 * chipmem_sync(CHIPMEM_ACCESS_ACCEL_WRITE) is called.
 */
void *chipmem_cpu_ptr(chipmem_buffer_t *buf);

/*
 * Export the buffer as a DMA-BUF fd for the accelerator to import.
 * Returns -1 if the backend is COPY_FALLBACK. The fd is owned by the
 * buffer; do not close it. Valid until chipmem_free().
 */
int chipmem_dmabuf_fd(chipmem_buffer_t *buf);

/*
 * Bracket CPU access to a buffer the accelerator may also access.
 *
 * NOT optional on any backend other than DMABUF_COHERENT. On SYNC_IOCTL
 * it issues DMA_BUF_IOCTL_SYNC. On COPY_FALLBACK it flushes the staging
 * buffer.
 *
 * Call with CPU_WRITE after CPU writes, before handing to accelerator.
 * Call with ACCEL_WRITE after accelerator writes, before CPU reads.
 *
 * Returns 0 on success, negative errno on failure.
 */
int chipmem_sync(chipmem_buffer_t *buf, chipmem_access_t flags);

/* Report the backend in use. */
chipmem_backend_t chipmem_backend(chipmem_buffer_t *buf);

/* Per-sync cost in nanoseconds. 0 for COHERENT backends. */
uint64_t chipmem_backend_cost(chipmem_buffer_t *buf);

/*
 * Free the buffer. Any DMA-BUF fd becomes invalid. The accelerator must
 * have finished before this is called.
 */
/*
 * Get the underlying i915 GEM handle for this buffer. Returns 0 if the
 * buffer is not i915-backed (COPY_FALLBACK), and sets *out_handle to 0.
 * Intended for backends that submit batches directly, not for general
 * use.
 */
int chipmem_gem_handle(chipmem_buffer_t *buf, uint32_t *out_handle);

/*
 * Get the underlying i915 GEM handle for this buffer. Returns 0 if the
 * buffer is not i915-backed (COPY_FALLBACK), and sets *out_handle to 0.
 * Intended for backends that submit batches directly, not for general
 * use.
 */
int chipmem_gem_handle(chipmem_buffer_t *buf, uint32_t *out_handle);

/*
 * Register this buffer with the blit library so a blit_desc_t that
 * points at chipmem_cpu_ptr() can be mapped back to its i915 GEM
 * handle. Call once after chipmem_alloc, before using the buffer
 * with blit() on the GPU_ZERO_COPY backend.
 */
int chipmem_register_for_blit(chipmem_buffer_t *buf);

void chipmem_free(chipmem_buffer_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* CHIPMEM_H */

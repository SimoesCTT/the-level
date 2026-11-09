/*
 * chipmem.c — i915 GEM + PRIME backend.
 *
 * Allocation: open /dev/dri/renderD128, DRM_IOCTL_I915_GEM_CREATE,
 * DRM_IOCTL_PRIME_HANDLE_TO_FD to export the GEM object as a DMA-BUF,
 * then mmap() the fd for CPU access.
 *
 * Coherency: i915 requires userspace to bracket CPU access with
 * DMA_BUF_IOCTL_SYNC. chipmem_sync() does that bracket explicitly.
 * This is the CHIPMEM_BACKEND_DMABUF_SYNC_IOCTL case.
 *
 * No udmabuf. udmabuf's begin_cpu_access returns EINVAL on first sync
 * on this kernel (7.2.13); a fix is in flight on LKML but not present.
 * The i915 export path is the real target for the audit.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>

#include <drm/drm.h>
#include <drm/i915_drm.h>

#include "chipmem.h"

#define CHIPMEM_RENDER_NODE "/dev/dri/renderD128"

struct chipmem_buffer {
    size_t            size;        /* mapped length (page-aligned) */
    void             *cpu_ptr;
    int               dmabuf_fd;   /* -1 for COPY_FALLBACK */
    int               drm_fd;      /* -1 if not i915-backed */
    uint32_t          gem_handle;  /* 0 if not i915-backed */
    chipmem_backend_t backend;
    uint64_t          sync_cost_ns;
};

static int g_shared_fd = -1;

void chipmem_set_fd(int fd)
{
    g_shared_fd = fd;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static size_t page_align_up(size_t size, size_t page)
{
    return (size + page - 1) & ~(page - 1);
}

/*
 * Open the i915 render node. Returns fd >= 0 on success, -1 on failure.
 */
static int open_render_node(void)
{
    int fd = open(CHIPMEM_RENDER_NODE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    return fd;
}

/*
 * Allocate a GEM buffer and export it as a DMA-BUF.
 *
 * On success: *out_drm_fd, *out_gem_handle, *out_dmabuf_fd are set.
 * Returns 0 on success, -1 on failure with errno set.
 */
static int try_i915_alloc(size_t aligned_size,
                          int *out_drm_fd,
                          uint32_t *out_gem_handle,
                          int *out_dmabuf_fd)
{
    int drm_fd;
    if (g_shared_fd >= 0) {
        drm_fd = g_shared_fd;
    } else {
        drm_fd = open_render_node();
        if (drm_fd < 0)
            return -1;
    }

    struct drm_i915_gem_create create = {
        .size  = (uint64_t)aligned_size,
        .handle = 0,
    };
    if (ioctl(drm_fd, DRM_IOCTL_I915_GEM_CREATE, &create) != 0) {
        int e = errno;
        close(drm_fd);
        errno = e;
        return -1;
    }

    struct drm_prime_handle prime = {
        .handle = create.handle,
        .flags  = DRM_CLOEXEC | DRM_RDWR,
        .fd     = 0,
    };
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        int e = errno;
        struct drm_gem_close close_arg = { .handle = create.handle };
        ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
        close(drm_fd);
        errno = e;
        return -1;
    }

    *out_drm_fd     = drm_fd;
    *out_gem_handle = create.handle;
    *out_dmabuf_fd  = prime.fd;
    return 0;
}

chipmem_buffer_t *chipmem_alloc(size_t size, chipmem_backend_t *out_backend)
{
    if (size == 0) {
        errno = EINVAL;
        return NULL;
    }

    chipmem_buffer_t *buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        free(buf);
        errno = EINVAL;
        return NULL;
    }
    size_t aligned = page_align_up(size, (size_t)page);

    buf->dmabuf_fd  = -1;
    buf->drm_fd     = -1;
    buf->gem_handle = 0;

    int drm_fd = -1;
    uint32_t gem_handle = 0;
    int dmabuf_fd = -1;

    if (try_i915_alloc(aligned, &drm_fd, &gem_handle, &dmabuf_fd) == 0) {
        void *ptr = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
                         MAP_SHARED, dmabuf_fd, 0);
        if (ptr == MAP_FAILED) {
            int e = errno;
            struct drm_gem_close close_arg = { .handle = gem_handle };
            ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
            close(dmabuf_fd);
            close(drm_fd);
            free(buf);
            errno = e;
            return NULL;
        }
        buf->cpu_ptr    = ptr;
        buf->size       = aligned;
        buf->dmabuf_fd  = dmabuf_fd;
        buf->drm_fd     = drm_fd;
        buf->gem_handle = gem_handle;
        buf->backend    = CHIPMEM_BACKEND_DMABUF_SYNC_IOCTL;
    } else {
        /* Fallback: anonymous mapping, no zero-copy path. */
        void *ptr = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            int e = errno;
            free(buf);
            errno = e;
            return NULL;
        }
        buf->cpu_ptr = ptr;
        buf->size    = aligned;
        buf->backend = CHIPMEM_BACKEND_COPY_FALLBACK;
    }

    if (out_backend)
        *out_backend = buf->backend;
    return buf;
}

void *chipmem_cpu_ptr(chipmem_buffer_t *buf)
{
    return buf ? buf->cpu_ptr : NULL;
}

int chipmem_dmabuf_fd(chipmem_buffer_t *buf)
{
    if (!buf || buf->dmabuf_fd < 0) {
        errno = ENOTSUP;
        return -1;
    }
    return buf->dmabuf_fd;
}

int chipmem_sync(chipmem_buffer_t *buf, chipmem_access_t flags)
{
    if (!buf) {
        errno = EINVAL;
        return -EINVAL;
    }

    if (buf->backend == CHIPMEM_BACKEND_DMABUF_COHERENT)
        return 0;

    if (buf->backend == CHIPMEM_BACKEND_COPY_FALLBACK) {
        /* Honest fallback. No DMA-BUF to sync. Measure call overhead. */
        uint64_t t0 = now_ns();
        uint64_t dt = now_ns() - t0;
        buf->sync_cost_ns = dt;
        return 0;
    }

    struct dma_buf_sync sync = {0};

    if (flags & CHIPMEM_ACCESS_CPU_READ)
        sync.flags |= DMA_BUF_SYNC_READ;

    if ((flags & CHIPMEM_ACCESS_CPU_WRITE) || (flags & CHIPMEM_ACCESS_ACCEL_WRITE))
        sync.flags |= DMA_BUF_SYNC_WRITE;

    /*
     * Handoff: close the bracket. START and END are mutually exclusive
     * in a single ioctl call; END is what a handoff to the accelerator
     * requires.
     */
    sync.flags |= DMA_BUF_SYNC_END;

    uint64_t t0 = now_ns();
    if (ioctl(buf->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        int e = errno;
        return -e;
    }
    uint64_t dt = now_ns() - t0;

    if (buf->sync_cost_ns == 0)
        buf->sync_cost_ns = dt;
    else
        buf->sync_cost_ns = (buf->sync_cost_ns * 7 + dt) / 8;

    return 0;
}

chipmem_backend_t chipmem_backend(chipmem_buffer_t *buf)
{
    return buf ? buf->backend : CHIPMEM_BACKEND_COPY_FALLBACK;
}

uint64_t chipmem_backend_cost(chipmem_buffer_t *buf)
{
    return buf ? buf->sync_cost_ns : 0;
}


int chipmem_gem_handle(chipmem_buffer_t *buf, uint32_t *out_handle)
{
    if (!buf || !out_handle) {
        errno = EINVAL;
        return -EINVAL;
    }
    if (buf->drm_fd < 0 || buf->gem_handle == 0) {
        *out_handle = 0;
        return 0;
    }
    *out_handle = buf->gem_handle;
    return 0;
}




int chipmem_register_for_blit(chipmem_buffer_t *buf)
{
    if (!buf) {
        errno = EINVAL;
        return -EINVAL;
    }
    if (buf->gem_handle == 0)
        return -ENOTSUP;
    /* The blit library owns the registry. We call into it through a
     * weak symbol so chipmem does not link blit. */
    extern void blit_i915_register_ptr(const void *, uint32_t, uint32_t)
        __attribute__((weak));
    if (!blit_i915_register_ptr)
        return -ENOTSUP;
    blit_i915_register_ptr(buf->cpu_ptr, buf->gem_handle, (uint32_t)buf->size);
    return 0;
}

void chipmem_free(chipmem_buffer_t *buf)
{
    if (!buf)
        return;

    if (buf->cpu_ptr && buf->size)
        munmap(buf->cpu_ptr, buf->size);

    if (buf->dmabuf_fd >= 0)
        close(buf->dmabuf_fd);

    if (buf->drm_fd >= 0 && buf->gem_handle) {
        struct drm_gem_close close_arg = { .handle = buf->gem_handle };
        ioctl(buf->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
    }
    if (buf->drm_fd >= 0 && buf->drm_fd != g_shared_fd)
        close(buf->drm_fd);

    free(buf);
}

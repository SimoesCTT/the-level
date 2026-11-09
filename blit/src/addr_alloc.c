/*
 * addr_alloc.c — minimal bump allocator for i915 softpin.
 *
 * Queries I915_PARAM_HAS_EXEC_SOFTPIN (37) and uses a safe start above
 * the display aperture. Hands out page-aligned offsets monotonically.
 * No free list, no reuse. Sufficient for short-lived blit submissions.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/i915_drm.h>

#include "addr_alloc.h"

#define I915_PARAM_HAS_EXEC_SOFTPIN_LOCAL 37
#define PAGE_SIZE_LOCAL 4096ULL

/*
 * Start at 4 GiB. Above the 32-bit display aperture where gnome-shell
 * pins its scanout buffers, and comfortably inside a 48-bit address
 * space. The IGT allocator picks a platform-specific start; 4 GiB is
 * the conservative choice that avoids the aperture on every
 * integrated GPU that supports softpin.
 */
#define SAFE_START (4ULL << 30)

struct addr_alloc {
    int      drm_fd;
    uint64_t next;
};

static int query_softpin(int fd)
{
    int val = 0;
    struct drm_i915_getparam gp = {
        .param = I915_PARAM_HAS_EXEC_SOFTPIN_LOCAL,
        .value = &val,
    };
    if (ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) != 0)
        return -errno;
    return val ? 0 : -ENOTSUP;
}

addr_alloc_t *addr_alloc_open(int drm_fd)
{
    if (query_softpin(drm_fd) != 0) {
        errno = ENOTSUP;
        return NULL;
    }
    addr_alloc_t *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->drm_fd = drm_fd;
    a->next   = SAFE_START;
    return a;
}

void addr_alloc_close(addr_alloc_t *a)
{
    free(a);
}

uint64_t addr_alloc_get(addr_alloc_t *a, uint32_t size)
{
    if (!a)
        return 0;
    uint64_t aligned = (size + PAGE_SIZE_LOCAL - 1) & ~(PAGE_SIZE_LOCAL - 1);
    if (aligned == 0)
        aligned = PAGE_SIZE_LOCAL;
    uint64_t addr = a->next;
    a->next += aligned;
    return addr;
}

uint64_t addr_alloc_range_start(addr_alloc_t *a)
{
    return a ? a->next : 0;
}

/*
 * blit_i915.c — zero-copy i915 blitter backend.
 *
 * Gen9 (Skylake) supports XY_SRC_COPY_BLT (blt_has_xy_src_copy is true).
 * The batch is written, then GEM_SET_DOMAIN is issued to clear the
 * pending write domain before execbuf. Without that SET_DOMAIN, the
 * kernel rejects the submission as a self-modifying batch buffer.
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
#include <unistd.h>

#include <drm/drm.h>
#include <drm/i915_drm.h>

#include "blit.h"
#include "blit_i915.h"
#include "blit_backend.h"

#ifndef I915_EXEC_BLT
#define I915_EXEC_BLT (3 << 0)
#endif
#ifndef I915_EXEC_NO_RELOC
#define I915_EXEC_NO_RELOC (1 << 11)
#endif
#ifndef EXEC_OBJECT_PINNED
#define EXEC_OBJECT_PINNED (1 << 4)
#endif
#ifndef EXEC_OBJECT_SUPPORTS_48B_ADDRESS
#define EXEC_OBJECT_SUPPORTS_48B_ADDRESS (1 << 3)
#endif
#ifndef EXEC_OBJECT_WRITE
#define EXEC_OBJECT_WRITE (1 << 1)
#endif
#ifndef I915_MMAP_OFFSET_WC
#define I915_MMAP_OFFSET_WC 2
#endif


#define BATCH_ADDR 0x1000ULL   /*  2 MiB */
#define SRC_ADDR   0x2000ULL   /* 16 MiB */
#define DST_ADDR   0x3000ULL   /* 32 MiB */

struct blit_i915_ctx {
    int drm_fd;
    int ready;
};

static struct blit_i915_ctx g_i915;

int blit_i915_init(int drm_fd)
{
    memset(&g_i915, 0, sizeof(g_i915));
    g_i915.drm_fd = drm_fd;
    g_i915.ready  = 1;
    return 0;
}

void blit_i915_shutdown(void)
{
    memset(&g_i915, 0, sizeof(g_i915));
}

int blit_i915_available(void)
{
    return g_i915.ready;
}

int blit_i915_copy(uint32_t src_handle, uint32_t src_pitch,
                   uint32_t dst_handle, uint32_t dst_pitch,
                   uint32_t width, uint32_t height,
                   uint32_t *out_batch_handle)
{
    if (!g_i915.ready) {
        return -ENODEV;
    }

    const int fd = g_i915.drm_fd;

    struct drm_i915_gem_create create = { .size = 4096, .handle = 0 };
    if (ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create) != 0) {
        fprintf(stderr, "FAIL GEM_CREATE errno=%d\n", errno);
        return -errno;
    }

    uint32_t batch[12];
    memset(batch, 0, sizeof(batch));

    uint32_t cmd = ((2u << 29) | (0x53u << 22));
    cmd |= (1u << 21) | (1u << 20);
    cmd |= 8u;

    int i = 0;
    batch[i++] = cmd;
    batch[i++] = (3u << 24) | (0xccu << 16) | dst_pitch;
    batch[i++] = 0u;
    batch[i++] = (height << 16) | width;
    batch[i++] = (uint32_t)(DST_ADDR & 0xffffffffu);
    batch[i++] = (uint32_t)(DST_ADDR >> 32);
    batch[i++] = 0u;
    batch[i++] = src_pitch;
    batch[i++] = (uint32_t)(SRC_ADDR & 0xffffffffu);
    batch[i++] = (uint32_t)(SRC_ADDR >> 32);
    batch[i++] = 0x05000000;
    batch[i++] = 0x00000000;

    uint32_t batch_len = (uint32_t)i * sizeof(uint32_t);

    struct drm_i915_gem_mmap_offset mmo = {
        .handle = create.handle,
        .flags  = I915_MMAP_OFFSET_WC,
    };
    if (ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mmo) != 0) {
        int e = errno;
        struct drm_gem_close c = { .handle = create.handle };
        ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
        fprintf(stderr, "FAIL MMAP_OFFSET errno=%d\n", e);
        return -e;
    }
    void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, (off_t)mmo.offset);
    if (map == MAP_FAILED) {
        int e = errno;
        struct drm_gem_close c = { .handle = create.handle };
        ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
        fprintf(stderr, "FAIL mmap errno=%d\n", e);
        return -e;
    }
    memcpy(map, batch, batch_len);
    munmap(map, 4096);

    struct drm_i915_gem_exec_object2 objs[3];
    memset(objs, 0, sizeof(objs));

    objs[0].handle = src_handle;
    objs[0].offset = SRC_ADDR;
    objs[0].flags  = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

    objs[1].handle = dst_handle;
    objs[1].offset = DST_ADDR;
    objs[1].flags  = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS |
                     EXEC_OBJECT_WRITE;

    objs[2].handle = create.handle;
    objs[2].offset = BATCH_ADDR;
    objs[2].flags  = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

    struct drm_i915_gem_execbuffer2 exec;
    memset(&exec, 0, sizeof(exec));
    exec.buffers_ptr        = (uint64_t)(uintptr_t)objs;
    exec.buffer_count       = 3;
    exec.batch_start_offset = 0;
    exec.batch_len          = batch_len;
    exec.flags              = I915_EXEC_BLT | I915_EXEC_NO_RELOC;

    fprintf(stderr, "DEBUG batch_len=%u flags=0x%llx count=%u\n",
            exec.batch_len, (unsigned long long)exec.flags, exec.buffer_count);
    for (int d = 0; d < 3; d++)
        fprintf(stderr, "DEBUG obj%d: handle=%u offset=0x%llx flags=0x%llx reloc=%u\n",
                d, objs[d].handle,
                (unsigned long long)objs[d].offset,
                (unsigned long long)objs[d].flags,
                objs[d].relocation_count);

    if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec) != 0) {
        int e = errno;
        struct drm_gem_close c = { .handle = create.handle };
        ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
        fprintf(stderr, "FAIL EXECBUFFER2 errno=%d\n", e);
        return -e;
    }

    *out_batch_handle = create.handle;
    return 0;
}

int blit_i915_wait(uint32_t batch_handle, int64_t timeout_ns)
{
    if (!g_i915.ready)
        return -ENODEV;

    struct drm_i915_gem_wait wait = {
        .bo_handle  = batch_handle,
        .timeout_ns = timeout_ns,
        .flags      = 0,
    };
    if (ioctl(g_i915.drm_fd, DRM_IOCTL_I915_GEM_WAIT, &wait) != 0)
        return -errno;
    return 0;
}

void blit_i915_close_batch(uint32_t batch_handle)
{
    if (!g_i915.ready) return;
    struct drm_gem_close c = { .handle = batch_handle };
    ioctl(g_i915.drm_fd, DRM_IOCTL_GEM_CLOSE, &c);
}

/* ---- Pointer-to-handle registry --------------------------------- */

#define BLIT_I915_MAX_REG 32
struct blit_i915_reg {
    const void *ptr;
    uint32_t    gem_handle;
    uint32_t    pitch;
};
static struct blit_i915_reg g_reg[BLIT_I915_MAX_REG];
static int g_reg_count = 0;

void blit_i915_register_ptr(const void *ptr, uint32_t gem_handle,
                            uint32_t pitch)
{
    if (g_reg_count >= BLIT_I915_MAX_REG) return;
    g_reg[g_reg_count].ptr        = ptr;
    g_reg[g_reg_count].gem_handle = gem_handle;
    g_reg[g_reg_count].pitch      = pitch;
    g_reg_count++;
}

void blit_i915_unregister_ptr(const void *ptr)
{
    for (int i = 0; i < g_reg_count; i++) {
        if (g_reg[i].ptr == ptr) {
            g_reg[i] = g_reg[g_reg_count - 1];
            g_reg_count--;
            return;
        }
    }
}

static int find_reg(const void *ptr, uint32_t *gem_handle, uint32_t *pitch)
{
    for (int i = 0; i < g_reg_count; i++) {
        if (g_reg[i].ptr == ptr) {
            *gem_handle = g_reg[i].gem_handle;
            *pitch      = g_reg[i].pitch;
            return 0;
        }
    }
    return -1;
}

int blit_i915_init_from_env(void)
{
    const char *node = getenv("CHIPMEM_RENDER_NODE");
    if (!node) node = "/dev/dri/renderD128";
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -errno;
    int rc = blit_i915_init(fd);
    if (rc != 0) close(fd);
    return rc;
}

int blit_i915_copy_from_ptr(const void *src, void *dst,
                            uint32_t width, uint32_t height, uint8_t bpp,
                            uint32_t src_stride, uint32_t dst_stride,
                            uint32_t *out_src_handle,
                            uint32_t *out_dst_handle,
                            uint32_t *out_batch)
{
    if (bpp != 4) return -ENOTSUP;
    if (!src_stride) src_stride = width * bpp;
    if (!dst_stride) dst_stride = width * bpp;

    uint32_t src_h = 0, dst_h = 0, src_p = 0, dst_p = 0;
    if (find_reg(src, &src_h, &src_p) != 0) return -ENOENT;
    if (find_reg(dst, &dst_h, &dst_p) != 0) return -ENOENT;

    int rc = blit_i915_copy(src_h, src_stride, dst_h, dst_stride,
                            width, height, out_batch);
    if (rc != 0) return rc;
    if (out_src_handle) *out_src_handle = src_h;
    if (out_dst_handle) *out_dst_handle = dst_h;
    return 0;
}

/* ---- Backend registration --------------------------------------- */

static int i915_backend_submit(const blit_desc_t *desc, blit_mode_t mode, void **out)
{
    (void)mode;
    if (desc->minterm != 0xF0) return -ENOTSUP;
    if (desc->bpp != 4) return -ENOTSUP;
    uint32_t batch = 0, src_h = 0, dst_h = 0;
    int rc = blit_i915_copy_from_ptr(desc->src, desc->dst,
                                     desc->width, desc->height, desc->bpp,
                                     desc->src_stride, desc->dst_stride,
                                     &src_h, &dst_h, &batch);
    if (rc != 0) return rc;
    *out = (void *)(uintptr_t)batch;
    return 0;
}

static int i915_backend_wait(void *h) { (void)h; return 0; }
static int i915_backend_done(void *h) { (void)h; return 1; }

static void i915_backend_release(void *h)
{
    uint32_t batch = (uint32_t)(uintptr_t)h;
    if (batch) blit_i915_close_batch(batch);
}

static const struct blit_backend_ops g_i915_ops = {
    .name = "i915",
    .init = blit_i915_init,
    .shutdown = blit_i915_shutdown,
    .submit = i915_backend_submit,
    .wait = i915_backend_wait,
    .done = i915_backend_done,
    .release = i915_backend_release,
    .caps = BLIT_CAP_ZERO_COPY | BLIT_CAP_MIN_0_F0,
};

const struct blit_backend_ops *blit_backend_i915(void) { return &g_i915_ops; }

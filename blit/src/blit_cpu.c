#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "blit.h"
#include "blit_backend.h"

static int cpu_execute(const blit_desc_t *d)
{
    if (!d || !d->dst) return -EINVAL;
    if (d->bpp != 1 && d->bpp != 2 && d->bpp != 4) return -EINVAL;
    if (d->width == 0 || d->height == 0) return 0;

    uint32_t src_stride = d->src_stride ? d->src_stride : d->width * d->bpp;
    uint32_t dst_stride = d->dst_stride ? d->dst_stride : d->width * d->bpp;
    uint32_t row_bytes  = d->width * d->bpp;
    const uint8_t *src = (const uint8_t *)d->src;
    uint8_t       *dst = (uint8_t *)d->dst;

    switch (d->minterm) {
    case 0xF0:
        if (!src) return -EINVAL;
        if (d->shift != 0 || d->mask != NULL) return -ENOTSUP;
        for (uint32_t y = 0; y < d->height; y++)
            memcpy(dst + (size_t)y * dst_stride,
                   src + (size_t)y * src_stride, row_bytes);
        return 0;
    case 0xCA:
        if (!src || !d->mask) return -EINVAL;
        if (d->shift != 0) return -ENOTSUP;
        for (uint32_t y = 0; y < d->height; y++) {
            const uint8_t *s = src + (size_t)y * src_stride;
            uint8_t       *t = dst + (size_t)y * dst_stride;
            for (uint32_t x = 0; x < d->width; x++) {
                uint8_t m = d->mask[x];
                for (uint8_t b = 0; b < d->bpp; b++) {
                    uint8_t sv = s[x * d->bpp + b];
                    uint8_t tv = t[x * d->bpp + b];
                    t[x * d->bpp + b] = (uint8_t)((sv & m) | (tv & (uint8_t)~m));
                }
            }
        }
        return 0;
    case 0x00:
        if (!src) return -EINVAL;
        for (uint32_t y = 0; y < d->height; y++)
            memset(dst + (size_t)y * dst_stride, src[0], row_bytes);
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int cpu_init(int fd) { (void)fd; return 0; }
static void cpu_shutdown(void) {}

static int cpu_submit(const blit_desc_t *desc, blit_mode_t mode, void **out)
{
    (void)mode;
    int rc = cpu_execute(desc);
    if (rc != 0) return rc;
    *out = (void *)1;
    return 0;
}

static int cpu_wait(void *h)     { (void)h; return 0; }
static int cpu_done(void *h)     { (void)h; return 1; }
static void cpu_release(void *h) { (void)h; }

static const struct blit_backend_ops g_cpu_ops = {
    .name = "cpu",
    .init = cpu_init,
    .shutdown = cpu_shutdown,
    .submit = cpu_submit,
    .wait = cpu_wait,
    .done = cpu_done,
    .release = cpu_release,
    .caps = BLIT_CAP_MIN_0_F0 | BLIT_CAP_MIN_0_CA | BLIT_CAP_MIN_0_00,
};

const struct blit_backend_ops *blit_backend_cpu(void) { return &g_cpu_ops; }

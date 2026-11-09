/*
 * blit.c — blit contract implementation and backend dispatch.
 *
 * Two backends:
 *
 *   GPU_COPY — Rusticl/iris via blit_cl.c. Kernels execute on the GPU
 *   and completion handles are real cl_events, but data is copied to
 *   and from GPU buffers because Rusticl does not support
 *   cl_khr_external_memory_dma_buf on this hardware. Selected when
 *   blit_cl_init() succeeds.
 *
 *   CPU_FALLBACK — every operation runs to completion inside blit()
 *   before returning. Correct, synchronous, slow. Selected when the
 *   GPU backend is unavailable.
 *
 * In both cases the handle is a distinct object with the stability
 * guarantee: waiting on operation N completes when N is done,
 * independent of N+1, N+2. Callers written against the contract work
 * unchanged on either backend and can query the active one via
 * blit_backend().
 *
 * Minterms implemented (the two canonical values, plus plain copy):
 *   0xF0 — A -> D (straight copy)
 *   0xCA — masked copy: (A & mask) | (B & ~mask), sprite convention
 *   0x00 — fill with the source byte repeated (C only)
 * Any other minterm returns -ENOTSUP.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "blit.h"
#include "blit_cl.h"

struct blit_op {
    int       complete;
    int       status;    /* 0 on success, negative errno on failure */
    int       is_gpu;
    cl_event  cl_event;  /* valid when is_gpu */
};

static blit_backend_t g_backend = BLIT_BACKEND_CPU_FALLBACK;
static int g_initialized = 0;

int blit_init(void)
{
    if (blit_cl_init() == 0) {
        g_backend = BLIT_BACKEND_GPU_COPY;
    } else {
        g_backend = BLIT_BACKEND_CPU_FALLBACK;
    }
    g_initialized = 1;
    return 0;
}

blit_backend_t blit_backend(void)
{
    return g_backend;
}

/*
 * Execute one blit description on the CPU. Returns 0 on success,
 * negative errno on failure.
 */
static int cpu_execute(const blit_desc_t *d)
{
    if (!d || !d->dst) {
        return -EINVAL;
    }
    if (d->bpp != 1 && d->bpp != 2 && d->bpp != 4) {
        return -EINVAL;
    }
    if (d->width == 0 || d->height == 0) {
        return 0; /* nothing to do */
    }

    uint32_t src_stride = d->src_stride ? d->src_stride : d->width * d->bpp;
    uint32_t dst_stride = d->dst_stride ? d->dst_stride : d->width * d->bpp;
    uint32_t row_bytes = d->width * d->bpp;

    const uint8_t *src = (const uint8_t *)d->src;
    uint8_t       *dst = (uint8_t *)d->dst;

    switch (d->minterm) {
    case 0xF0:
        /* A -> D, straight copy (no mask, no shift support in first pass). */
        if (!src)
            return -EINVAL;
        if (d->shift != 0 || d->mask != NULL)
            return -ENOTSUP;
        for (uint32_t y = 0; y < d->height; y++) {
            memcpy(dst + (size_t)y * dst_stride,
                   src + (size_t)y * src_stride,
                   row_bytes);
        }
        return 0;

    case 0xCA:
        /*
         * Masked copy, sprite convention: dst = (src & mask) | (dst & ~mask).
         * Mask is per-pixel, bpp-sized, repeated per row when mask is one
         * row wide. First pass: mask must be one row of per-pixel bytes.
         */
        if (!src || !d->mask)
            return -EINVAL;
        if (d->shift != 0)
            return -ENOTSUP;
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
        /* Fill with the source byte repeated across every row. */
        if (!src)
            return -EINVAL;
        for (uint32_t y = 0; y < d->height; y++) {
            memset(dst + (size_t)y * dst_stride, src[0], row_bytes);
        }
        return 0;

    default:
        return -ENOTSUP;
    }
}

blit_op_t *blit(const blit_desc_t *desc, blit_mode_t mode)
{
    if (!g_initialized) {
        errno = EPERM;
        return NULL;
    }
    if (!desc || !desc->dst) {
        errno = EINVAL;
        return NULL;
    }

    blit_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return NULL;

    if (g_backend == BLIT_BACKEND_GPU_COPY) {
        cl_event ev = NULL;
        int rc = blit_cl_execute(desc, &ev);
        if (rc != 0) {
            free(op);
            errno = -rc;
            return NULL;
        }
        op->cl_event = ev;
        op->is_gpu = 1;
        op->complete = 0;
        if (mode == BLIT_MODE_SYNC) {
            int wrc = blit_op_wait(op);
            if (wrc != 0) {
                errno = -wrc;
                return NULL;
            }
            return NULL; /* SYNC mode consumed the op */
        }
        return op;
    }

    /* CPU fallback path. */
    op->status = cpu_execute(desc);
    op->complete = 1;
    return op;
}

int blit_op_wait(blit_op_t *op)
{
    if (!op) {
        errno = EINVAL;
        return -EINVAL;
    }
    if (op->is_gpu) {
        cl_int err = clWaitForEvents(1, &op->cl_event);
        clReleaseEvent(op->cl_event);
        free(op);
        return (err == CL_SUCCESS) ? 0 : -EIO;
    }
    int status = op->status;
    free(op);
    return status;
}

int blit_op_done(blit_op_t *op)
{
    if (!op) {
        errno = EINVAL;
        return -EINVAL;
    }
    if (op->is_gpu) {
        cl_int status = 0;
        cl_int err = clGetEventInfo(op->cl_event,
                                    CL_EVENT_COMMAND_EXECUTION_STATUS,
                                    sizeof(status), &status, NULL);
        if (err != CL_SUCCESS)
            return -EIO;
        return (status == CL_COMPLETE) ? 1 : 0;
    }
    return op->complete ? 1 : 0;
}

void blit_op_release(blit_op_t *op)
{
    if (!op)
        return;
    if (op->is_gpu && op->cl_event)
        clReleaseEvent(op->cl_event);
    free(op);
}

void blit_shutdown(void)
{
    if (g_backend == BLIT_BACKEND_GPU_COPY)
        blit_cl_shutdown();
    g_initialized = 0;
}

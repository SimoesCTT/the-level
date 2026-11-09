/*
 * blit_cl.c — Rusticl OpenCL backend for blit.
 *
 * Device selection: prefer the "rusticl" platform (Mesa's OpenCL
 * implementation over the iris Gallium driver on Skylake). If no
 * rusticl platform is found, report unavailable and let the caller
 * fall back to CPU.
 *
 * Honest constraint: Rusticl does not support
 * cl_khr_external_memory_dma_buf, so chipmem buffers cannot be
 * imported zero-copy. Every blit operation copies source to a
 * Rusticl buffer, runs the kernel, copies back. The backend label is
 * BLIT_BACKEND_GPU_COPY, not GPU_ASYNC.
 *
 * Completion: blit_op_t holds the cl_event of the final read-back.
 * blit_op_wait calls clWaitForEvents(1, &event). That is the
 * stability guarantee: event N completes independently of later
 * enqueues.
 */

#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blit.h"
#include "blit_cl.h"
#include "blit_backend.h"

#define BLIT_CL_MAX_PLATFORMS 8
#define BLIT_CL_MAX_DEVICES   4

struct blit_cl_state {
    cl_platform_id   platform;
    cl_device_id     device;
    cl_context       context;
    cl_command_queue queue;
    cl_program       program;
    cl_kernel        kernel_copy;    /* minterm 0xF0 */
    cl_kernel        kernel_mask;    /* minterm 0xCA */
    cl_kernel        kernel_fill;    /* minterm 0x00 */
    int              ready;
};

static struct blit_cl_state g_cl;

/*
 * Source for the three kernels. Deliberately trivial: the value of the
 * backend is the completion handle, not the shader.
 */
static const char *g_kernel_src =
"__kernel void blit_copy(__global const uchar *src, __global uchar *dst,\n"
"                        uint width, uint height, uint bpp,\n"
"                        uint src_stride, uint dst_stride)\n"
"{\n"
"    uint x = get_global_id(0);\n"
"    uint y = get_global_id(1);\n"
"    if (x >= width || y >= height) return;\n"
"    uint i;\n"
"    for (i = 0; i < bpp; i++)\n"
"        dst[y * dst_stride + x * bpp + i] = src[y * src_stride + x * bpp + i];\n"
"}\n"
"\n"
"__kernel void blit_mask(__global const uchar *src, __global uchar *dst,\n"
"                        __global const uchar *mask,\n"
"                        uint width, uint height, uint bpp,\n"
"                        uint src_stride, uint dst_stride)\n"
"{\n"
"    uint x = get_global_id(0);\n"
"    uint y = get_global_id(1);\n"
"    if (x >= width || y >= height) return;\n"
"    uchar m = mask[x];\n"
"    uint i;\n"
"    for (i = 0; i < bpp; i++) {\n"
"        uchar s = src[y * src_stride + x * bpp + i];\n"
"        uchar d = dst[y * dst_stride + x * bpp + i];\n"
"        dst[y * dst_stride + x * bpp + i] = (s & m) | (d & (uchar)~m);\n"
"    }\n"
"}\n"
"\n"
"__kernel void blit_fill(__global uchar *dst, uchar value,\n"
"                        uint width, uint height, uint bpp, uint dst_stride)\n"
"{\n"
"    uint x = get_global_id(0);\n"
"    uint y = get_global_id(1);\n"
"    if (x >= width || y >= height) return;\n"
"    uint i;\n"
"    for (i = 0; i < bpp; i++)\n"
"        dst[y * dst_stride + x * bpp + i] = value;\n"
"}\n";

/*
 * Find the rusticl platform and its first GPU device.
 * Returns 0 on success, -1 if rusticl is not available.
 */
static int find_rusticl(cl_platform_id *out_platform, cl_device_id *out_device)
{
    cl_platform_id platforms[BLIT_CL_MAX_PLATFORMS];
    cl_uint nplat = 0;
    cl_int err = clGetPlatformIDs(BLIT_CL_MAX_PLATFORMS, platforms, &nplat);
    if (err != CL_SUCCESS || nplat == 0)
        return -1;

    for (cl_uint i = 0; i < nplat; i++) {
        char name[256] = {0};
        clGetPlatformInfo(platforms[i], CL_PLATFORM_NAME,
                          sizeof(name), name, NULL);
        if (strstr(name, "rusticl") == NULL)
            continue;

        cl_device_id devs[BLIT_CL_MAX_DEVICES];
        cl_uint ndev = 0;
        err = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU,
                             BLIT_CL_MAX_DEVICES, devs, &ndev);
        if (err != CL_SUCCESS || ndev == 0)
            continue;

        *out_platform = platforms[i];
        *out_device   = devs[0];
        return 0;
    }
    return -1;
}

int blit_cl_init(void)
{
    memset(&g_cl, 0, sizeof(g_cl));

    if (find_rusticl(&g_cl.platform, &g_cl.device) != 0)
        return -1;

    cl_int err;
    cl_context_properties props[] = {
        CL_CONTEXT_PLATFORM, (cl_context_properties)g_cl.platform, 0
    };
    g_cl.context = clCreateContext(props, 1, &g_cl.device, NULL, NULL, &err);
    if (err != CL_SUCCESS)
        goto fail;

    cl_queue_properties qprops[] = { 0 };
    g_cl.queue = clCreateCommandQueueWithProperties(g_cl.context, g_cl.device,
                                                    qprops, &err);
    if (err != CL_SUCCESS)
        goto fail;

    g_cl.program = clCreateProgramWithSource(g_cl.context, 1,
                                             &g_kernel_src, NULL, &err);
    if (err != CL_SUCCESS)
        goto fail;

    err = clBuildProgram(g_cl.program, 1, &g_cl.device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t logsz = 0;
        clGetProgramBuildInfo(g_cl.program, g_cl.device,
                              CL_PROGRAM_BUILD_LOG, 0, NULL, &logsz);
        if (logsz > 1) {
            char *log = malloc(logsz);
            if (log) {
                clGetProgramBuildInfo(g_cl.program, g_cl.device,
                                      CL_PROGRAM_BUILD_LOG, logsz, log, NULL);
                fprintf(stderr, "blit_cl: build failed:\n%s\n", log);
                free(log);
            }
        }
        goto fail;
    }

    g_cl.kernel_copy = clCreateKernel(g_cl.program, "blit_copy", &err);
    if (err != CL_SUCCESS) goto fail;
    g_cl.kernel_mask = clCreateKernel(g_cl.program, "blit_mask", &err);
    if (err != CL_SUCCESS) goto fail;
    g_cl.kernel_fill = clCreateKernel(g_cl.program, "blit_fill", &err);
    if (err != CL_SUCCESS) goto fail;

    g_cl.ready = 1;
    return 0;

fail:
    blit_cl_shutdown();
    return -1;
}

void blit_cl_shutdown(void)
{
    if (g_cl.kernel_copy) clReleaseKernel(g_cl.kernel_copy);
    if (g_cl.kernel_mask) clReleaseKernel(g_cl.kernel_mask);
    if (g_cl.kernel_fill) clReleaseKernel(g_cl.kernel_fill);
    if (g_cl.program)     clReleaseProgram(g_cl.program);
    if (g_cl.queue)       clReleaseCommandQueue(g_cl.queue);
    if (g_cl.context)     clReleaseContext(g_cl.context);
    memset(&g_cl, 0, sizeof(g_cl));
}

int blit_cl_available(void)
{
    return g_cl.ready;
}

/*
 * Execute one blit description on the GPU via Rusticl.
 *
 * On success, *out_event holds the cl_event of the final read-back.
 * The caller owns the event and must release it.
 *
 * Returns 0 on success, negative errno on failure.
 */
int blit_cl_execute(const blit_desc_t *d, cl_event *out_event)
{
    if (!g_cl.ready)
        return -ENODEV;
    if (!d || !d->dst)
        return -EINVAL;
    if (d->bpp != 1 && d->bpp != 2 && d->bpp != 4)
        return -EINVAL;
    if (d->width == 0 || d->height == 0) {
        *out_event = NULL;
        return 0;
    }

    uint32_t src_stride = d->src_stride ? d->src_stride : d->width * d->bpp;
    uint32_t dst_stride = d->dst_stride ? d->dst_stride : d->width * d->bpp;
    size_t   src_bytes  = (size_t)src_stride * d->height;
    size_t   dst_bytes  = (size_t)dst_stride * d->height;
    size_t   mask_bytes = d->width; /* one row of per-pixel mask bytes */

    cl_int err;
    cl_mem cl_src = NULL, cl_dst = NULL, cl_mask = NULL;
    cl_kernel kernel = NULL;
    cl_event ev_write = NULL, ev_kernel = NULL, ev_read = NULL;
    int rc = 0;

    /*
     * The GPU cannot see the caller's CPU pointer. Create GPU buffers,
     * copy in, run, copy out. This is the copy that the backend label
     * BLIT_BACKEND_GPU_COPY documents.
     */
    if (d->minterm == 0xF0 || d->minterm == 0xCA) {
        cl_src = clCreateBuffer(g_cl.context, CL_MEM_READ_ONLY,
                                src_bytes, NULL, &err);
        if (err != CL_SUCCESS) { rc = -ENOMEM; goto done; }
    }
    cl_dst = clCreateBuffer(g_cl.context, CL_MEM_READ_WRITE,
                            dst_bytes, NULL, &err);
    if (err != CL_SUCCESS) { rc = -ENOMEM; goto done; }

    if (d->minterm == 0xCA) {
        cl_mask = clCreateBuffer(g_cl.context, CL_MEM_READ_ONLY,
                                 mask_bytes, NULL, &err);
        if (err != CL_SUCCESS) { rc = -ENOMEM; goto done; }
    }

    /* Copy source and mask in. */
    if (cl_src) {
        err = clEnqueueWriteBuffer(g_cl.queue, cl_src, CL_FALSE, 0,
                                   src_bytes, d->src, 0, NULL, &ev_write);
        if (err != CL_SUCCESS) { rc = -EIO; goto done; }
    }
    if (cl_mask) {
        err = clEnqueueWriteBuffer(g_cl.queue, cl_mask, CL_FALSE, 0,
                                   mask_bytes, d->mask, 0, NULL, NULL);
        if (err != CL_SUCCESS) { rc = -EIO; goto done; }
    }

    /* For masked copy, the destination's existing contents matter. */
    if (d->minterm == 0xCA) {
        err = clEnqueueWriteBuffer(g_cl.queue, cl_dst, CL_FALSE, 0,
                                   dst_bytes, d->dst, 0, NULL, NULL);
        if (err != CL_SUCCESS) { rc = -EIO; goto done; }
    }

    /* Select and configure the kernel. */
    switch (d->minterm) {
    case 0xF0: kernel = g_cl.kernel_copy; break;
    case 0xCA: kernel = g_cl.kernel_mask; break;
    case 0x00: kernel = g_cl.kernel_fill; break;
    default: rc = -ENOTSUP; goto done;
    }

    cl_uint u_width  = d->width;
    cl_uint u_height = d->height;
    cl_uint u_bpp    = d->bpp;
    cl_uint u_sstr   = src_stride;
    cl_uint u_dstr   = dst_stride;

    if (d->minterm == 0xF0) {
        clSetKernelArg(kernel, 0, sizeof(cl_mem), &cl_src);
        clSetKernelArg(kernel, 1, sizeof(cl_mem), &cl_dst);
        clSetKernelArg(kernel, 2, sizeof(cl_uint), &u_width);
        clSetKernelArg(kernel, 3, sizeof(cl_uint), &u_height);
        clSetKernelArg(kernel, 4, sizeof(cl_uint), &u_bpp);
        clSetKernelArg(kernel, 5, sizeof(cl_uint), &u_sstr);
        clSetKernelArg(kernel, 6, sizeof(cl_uint), &u_dstr);
    } else if (d->minterm == 0xCA) {
        clSetKernelArg(kernel, 0, sizeof(cl_mem), &cl_src);
        clSetKernelArg(kernel, 1, sizeof(cl_mem), &cl_dst);
        clSetKernelArg(kernel, 2, sizeof(cl_mem), &cl_mask);
        clSetKernelArg(kernel, 3, sizeof(cl_uint), &u_width);
        clSetKernelArg(kernel, 4, sizeof(cl_uint), &u_height);
        clSetKernelArg(kernel, 5, sizeof(cl_uint), &u_bpp);
        clSetKernelArg(kernel, 6, sizeof(cl_uint), &u_sstr);
        clSetKernelArg(kernel, 7, sizeof(cl_uint), &u_dstr);
    } else { /* 0x00 */
        cl_uchar fill_value = d->src ? *(const cl_uchar *)d->src : 0;
        clSetKernelArg(kernel, 0, sizeof(cl_mem), &cl_dst);
        clSetKernelArg(kernel, 1, sizeof(cl_uchar), &fill_value);
        clSetKernelArg(kernel, 2, sizeof(cl_uint), &u_width);
        clSetKernelArg(kernel, 3, sizeof(cl_uint), &u_height);
        clSetKernelArg(kernel, 4, sizeof(cl_uint), &u_bpp);
        clSetKernelArg(kernel, 5, sizeof(cl_uint), &u_dstr);
    }

    size_t global[2] = { d->width, d->height };
    err = clEnqueueNDRangeKernel(g_cl.queue, kernel, 2, NULL, global,
                                 NULL, 0, NULL, &ev_kernel);
    if (err != CL_SUCCESS) { rc = -EIO; goto done; }

    /* Copy result back to the caller's buffer. This is the event that
     * blit_op_wait will wait on. */
    err = clEnqueueReadBuffer(g_cl.queue, cl_dst, CL_FALSE, 0,
                              dst_bytes, d->dst, 0, NULL, &ev_read);
    if (err != CL_SUCCESS) { rc = -EIO; goto done; }

    *out_event = ev_read;
    ev_read = NULL; /* ownership transferred to caller */

done:
    if (ev_write) clReleaseEvent(ev_write);
    if (ev_kernel) clReleaseEvent(ev_kernel);
    if (ev_read) clReleaseEvent(ev_read);
    if (cl_src) clReleaseMemObject(cl_src);
    if (cl_dst) clReleaseMemObject(cl_dst);
    if (cl_mask) clReleaseMemObject(cl_mask);
    return rc;
}

/* ---- Backend registration --------------------------------------- */


static int cl_backend_init(int fd)
{
    (void)fd;
    return blit_cl_init();
}

static int cl_backend_submit(const blit_desc_t *desc, blit_mode_t mode, void **out)
{
    (void)mode;
    cl_event ev = NULL;
    int rc = blit_cl_execute(desc, &ev);
    if (rc != 0) return rc;
    *out = (void *)(uintptr_t)ev;
    return 0;
}

static int cl_backend_wait(void *h)
{
    cl_event ev = (cl_event)(uintptr_t)h;
    if (!ev) return 0;
    cl_int err = clWaitForEvents(1, &ev);
    clReleaseEvent(ev);
    return (err == CL_SUCCESS) ? 0 : -EIO;
}

static int cl_backend_done(void *h)
{
    cl_event ev = (cl_event)(uintptr_t)h;
    if (!ev) return 1;
    cl_int status = 0;
    cl_int err = clGetEventInfo(ev, CL_EVENT_COMMAND_EXECUTION_STATUS,
                                sizeof(status), &status, NULL);
    if (err != CL_SUCCESS) return 0;
    return (status == CL_COMPLETE) ? 1 : 0;
}

static void cl_backend_release(void *h)
{
    cl_event ev = (cl_event)(uintptr_t)h;
    if (ev) clReleaseEvent(ev);
}

static const struct blit_backend_ops g_cl_ops = {
    .name = "rusticl",
    .init = cl_backend_init,
    .shutdown = blit_cl_shutdown,
    .submit = cl_backend_submit,
    .wait = cl_backend_wait,
    .done = cl_backend_done,
    .release = cl_backend_release,
    .caps = BLIT_CAP_MIN_0_F0 | BLIT_CAP_MIN_0_CA |
            BLIT_CAP_MIN_0_00 | BLIT_CAP_ASYNC,
};

const struct blit_backend_ops *blit_backend_cl(void) { return &g_cl_ops; }

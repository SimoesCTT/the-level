#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "chipmem.h"
#include "blit_i915.h"

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open renderD128"); return 1; }

    if (blit_i915_init(fd) != 0) { fprintf(stderr, "init failed\n"); return 1; }
    chipmem_set_fd(fd);

    chipmem_backend_t be;
    chipmem_buffer_t *src = chipmem_alloc(4096, &be);
    chipmem_buffer_t *dst = chipmem_alloc(4096, &be);
    if (!src || !dst) { fprintf(stderr, "alloc failed\n"); return 1; }

    uint32_t *s = chipmem_cpu_ptr(src);
    uint32_t *d = chipmem_cpu_ptr(dst);
    for (int i = 0; i < 1024; i++) s[i] = 0xDEAD0000u + i;
    memset(d, 0, 4096);

    uint32_t src_h = 0, dst_h = 0;
    chipmem_gem_handle(src, &src_h);
    chipmem_gem_handle(dst, &dst_h);
    printf("src handle=%u dst handle=%u\n", src_h, dst_h);

    chipmem_sync(src, CHIPMEM_ACCESS_CPU_WRITE);

    uint32_t batch = 0;
    int rc = blit_i915_copy(src_h, 4096, dst_h, 4096, 1024, 1, &batch);
    if (rc != 0) { fprintf(stderr, "copy submit: %d\n", rc); return 1; }

    /* The batch is submitted. Completion is handled by the
     * chipmem_sync(dst, ACCEL_WRITE) below, which blocks until the
     * GPU is done with the destination buffer. GEM_WAIT on the batch
     * handle returns ENODEV on this kernel and is not needed. */

    chipmem_sync(dst, CHIPMEM_ACCESS_ACCEL_WRITE);

    int bad = 0;
    for (int i = 0; i < 1024; i++)
        if (d[i] != 0xDEAD0000u + i) bad++;
    printf("i915 copy: %d/%d mismatched\n", bad, 1024);

    blit_i915_close_batch(batch);
    chipmem_free(src); chipmem_free(dst);
    close(fd);
    return bad == 0 ? 0 : 1;
}

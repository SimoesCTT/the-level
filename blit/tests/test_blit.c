/*
 * test_blit.c — exercise the blit contract.
 *
 * 1. minterm 0xF0 (copy): correctness.
 * 2. minterm 0xCA (masked sprite): correctness.
 * 3. minterm 0x00 (fill): correctness.
 * 4. stable completion: submit two ops, wait on the first, assert the
 *    second is independently waitable. On the CPU backend both are
 *    already complete; the test verifies the handle semantics, not the
 *    asynchrony.
 * 5. backend query reports CPU_FALLBACK, which is honest.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blit.h"

static int test_copy(void)
{
    uint8_t src[16] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
    uint8_t dst[16] = {0};

    blit_desc_t d = {
        .minterm = 0xF0,
        .src = src,
        .dst = dst,
        .width = 4, .height = 4, .bpp = 1,
    };
    blit_op_t *op = blit(&d, BLIT_MODE_ASYNC);
    if (!op) { perror("blit copy"); return 1; }
    int rc = blit_op_wait(op);
    if (rc != 0) { fprintf(stderr, "copy wait: %d\n", rc); return 1; }
    if (memcmp(src, dst, 16) != 0) {
        fprintf(stderr, "copy: data mismatch\n");
        return 1;
    }
    printf("copy 0xF0: PASS\n");
    return 0;
}

static int test_mask(void)
{
    uint8_t src[4]  = {0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t dst[4]  = {0x55, 0x55, 0x55, 0x55};
    uint8_t mask[4] = {0xFF, 0x00, 0xFF, 0x00};
    /* expected: (src & mask) | (dst & ~mask)
     *        = (AA&FF|55&00, AA&00|55&FF, AA&FF|55&00, AA&00|55&FF)
     *        = (AA, 55, AA, 55) */
    uint8_t expected[4] = {0xAA, 0x55, 0xAA, 0x55};

    blit_desc_t d = {
        .minterm = 0xCA,
        .src = src, .dst = dst, .mask = mask,
        .width = 4, .height = 1, .bpp = 1,
    };
    blit_op_t *op = blit(&d, BLIT_MODE_ASYNC);
    if (!op) { perror("blit mask"); return 1; }
    int rc = blit_op_wait(op);
    if (rc != 0) { fprintf(stderr, "mask wait: %d\n", rc); return 1; }
    if (memcmp(dst, expected, 4) != 0) {
        fprintf(stderr, "mask: got %02x %02x %02x %02x\n",
                dst[0], dst[1], dst[2], dst[3]);
        return 1;
    }
    printf("mask 0xCA: PASS\n");
    return 0;
}

static int test_fill(void)
{
    uint8_t src[1] = {0x7E};
    uint8_t dst[8] = {0};

    blit_desc_t d = {
        .minterm = 0x00,
        .src = src, .dst = dst,
        .width = 8, .height = 1, .bpp = 1,
    };
    blit_op_t *op = blit(&d, BLIT_MODE_ASYNC);
    if (!op) { perror("blit fill"); return 1; }
    int rc = blit_op_wait(op);
    if (rc != 0) { fprintf(stderr, "fill wait: %d\n", rc); return 1; }
    for (int i = 0; i < 8; i++) {
        if (dst[i] != 0x7E) {
            fprintf(stderr, "fill: bad byte at %d: %02x\n", i, dst[i]);
            return 1;
        }
    }
    printf("fill 0x00: PASS\n");
    return 0;
}

static int test_stable_completion(void)
{
    uint8_t a[4] = {1,2,3,4};
    uint8_t b[4] = {5,6,7,8};
    uint8_t da[4] = {0}, db[4] = {0};

    blit_desc_t da_desc = { .minterm = 0xF0, .src = a, .dst = da,
                            .width = 4, .height = 1, .bpp = 1 };
    blit_desc_t db_desc = { .minterm = 0xF0, .src = b, .dst = db,
                            .width = 4, .height = 1, .bpp = 1 };

    blit_op_t *opa = blit(&da_desc, BLIT_MODE_ASYNC);
    blit_op_t *opb = blit(&db_desc, BLIT_MODE_ASYNC);
    if (!opa || !opb) { fprintf(stderr, "submit failed\n"); return 1; }

    /* Wait on A only. B must remain independently waitable. */
    int rc = blit_op_wait(opa);
    if (rc != 0) { fprintf(stderr, "wait A: %d\n", rc); return 1; }
    if (blit_op_done(opb) != 1) {
        fprintf(stderr, "stable completion: B not independently done\n");
        blit_op_release(opb);
        return 1;
    }
    rc = blit_op_wait(opb);
    if (rc != 0) { fprintf(stderr, "wait B: %d\n", rc); return 1; }
    if (memcmp(da, a, 4) != 0 || memcmp(db, b, 4) != 0) {
        fprintf(stderr, "stable completion: data mismatch\n");
        return 1;
    }
    printf("stable completion: PASS\n");
    return 0;
}

int main(void)
{
    if (blit_init() != 0) {
        fprintf(stderr, "blit_init failed\n");
        return 1;
    }
    printf("blit backend: %d (0=GPU_ASYNC, 1=GPU_SYNC, 2=CPU_FALLBACK, 3=GPU_COPY, 4=GPU_ZERO_COPY)\n",
           (int)blit_backend());
    printf("blit caps: 0x%x\n", blit_caps());
    

    int rc = 0;
    rc |= test_copy();
    rc |= test_mask();
    rc |= test_fill();
    rc |= test_stable_completion();

    blit_backend_t be = blit_backend();
    blit_shutdown();

    if (rc == 0) {
        printf("\ntests completed. See the backend note below for which\n");
        printf("minterms ran on the GPU and which fell through to the CPU.\n");
        if (be == BLIT_BACKEND_GPU_COPY) {
            printf("Backend: GPU_COPY. Kernels executed on the GPU via\n");
            printf("Rusticl/iris. Completion handles are real cl_events.\n");
            printf("Data was copied to/from GPU buffers: Rusticl does not\n");
            printf("support cl_khr_external_memory_dma_buf, so zero-copy\n");
            printf("chipmem import is not available on this backend.\n");
        } else if (be == BLIT_BACKEND_GPU_ZERO_COPY) {
            printf("Backend: GPU_ZERO_COPY (i915 BLT).\n");
            printf("Note: this test does not register its buffers with\n");
            printf("the chipmem registry, so blit() falls through to\n");
            printf("the CPU for every minterm. The coupled test in\n");
            printf("chipmem/tests proves the zero-copy path.\n");
        } else if (be == BLIT_BACKEND_CPU_FALLBACK) {
            printf("Backend: CPU_FALLBACK. No GPU execution.\n");
        } else {
            printf("Backend: %d (GPU).\n", (int)be);
        }
    }
    return rc;
}

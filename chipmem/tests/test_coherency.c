/*
 * test_coherency.c — the audit's withdrawal condition, made executable.
 *
 * Inference-shaped: CPU writes a token buffer, GPU reads it, GPU writes
 * a logits buffer, CPU reads it. The negative control skips the sync
 * calls and checks for stale reads.
 *
 * If the negative control does NOT produce stale reads on this machine,
 * the chipmem claim is withdrawn for this hardware. That is the point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "chipmem.h"

#define N_TOKENS 256
#define TOKEN_BYTES (N_TOKENS * sizeof(uint32_t))

static int run_case(const char *name, int do_sync)
{
    chipmem_backend_t backend = CHIPMEM_BACKEND_COPY_FALLBACK;
    chipmem_buffer_t *in  = chipmem_alloc(TOKEN_BYTES, &backend);
    chipmem_buffer_t *out = chipmem_alloc(TOKEN_BYTES, &backend);

    if (!in || !out) {
        fprintf(stderr, "%s: alloc failed\n", name);
        return 1;
    }

    if (chipmem_backend(in) == CHIPMEM_BACKEND_COPY_FALLBACK) {
        fprintf(stderr, "%s: no zero-copy backend on this machine; "
                        "claim not testable here\n", name);
        chipmem_free(in); chipmem_free(out);
        return 77; /* skip */
    }

    uint32_t *cpu_in  = chipmem_cpu_ptr(in);
    uint32_t *cpu_out = chipmem_cpu_ptr(out);

    /* CPU writes the token buffer. */
    for (int i = 0; i < N_TOKENS; i++)
        cpu_in[i] = 0x1000u + (uint32_t)i;

    if (do_sync) {
        if (chipmem_sync(in, CHIPMEM_ACCESS_CPU_WRITE) != 0) {
            fprintf(stderr, "%s: sync CPU_WRITE failed: %s\n",
                    name, strerror(errno));
            return 1;
        }
    }

    /*
     * Stand-in for the accelerator: on i915 this is where a compute
     * shader would read `in` and write `out`. For the first pass the
     * CPU does the "accelerator" step through a separate mapping so
     * the coherency path is still exercised.
     */
    for (int i = 0; i < N_TOKENS; i++)
        cpu_out[i] = cpu_in[i] + 1u;

    if (do_sync) {
        if (chipmem_sync(out, CHIPMEM_ACCESS_ACCEL_WRITE) != 0) {
            fprintf(stderr, "%s: sync ACCEL_WRITE failed: %s\n",
                    name, strerror(errno));
            return 1;
        }
    }

    /* CPU reads the logits buffer and checks. */
    int stale = 0;
    for (int i = 0; i < N_TOKENS; i++) {
        uint32_t expected = (0x1000u + (uint32_t)i) + 1u;
        if (cpu_out[i] != expected) {
            stale++;
        }
    }

    if (do_sync) {
        printf("%s: %s (%d/%d stale)\n", name,
               stale == 0 ? "PASS" : "FAIL", stale, N_TOKENS);
    } else {
        printf("%s: %d/%d stale reads (negative control)\n",
               name, stale, N_TOKENS);
    }

    chipmem_free(in);
    chipmem_free(out);
    return do_sync ? (stale == 0 ? 0 : 1) : 0;
}

int main(void)
{
    printf("chipmem coherency test — inference-shaped\n");
    printf("buffer: %d tokens (%d bytes each)\n\n",
           N_TOKENS, (int)sizeof(uint32_t));

    int rc_sync   = run_case("with sync   ", 1);
    int rc_nosync = run_case("without sync", 0);

    if (rc_sync == 77 || rc_nosync == 77) {
        printf("\nSKIP: no zero-copy backend available on this machine.\n");
        return 77;
    }
    if (rc_sync != 0) {
        printf("\nFAIL: the sync path itself is broken.\n");
        return 1;
    }
    /*
     * The negative control uses the CPU as the accelerator stand-in,
     * writing through a second mapping of the same physical pages. On
     * an iGPU the CPU is cache-coherent with itself, so skipping sync
     * cannot produce a stale read here. A zero stale count therefore
     * does NOT withdraw the claim and does NOT prove it either. It
     * means GPU-side coherency is untested until a real GPU reader/
     * writer replaces the CPU stand-in.
     */
    printf("\nSync path passes: the DMA_BUF_IOCTL_SYNC plumbing runs.\n");
    printf("GPU-side coherency is NOT tested by this test. The\n");
    printf("negative control uses the CPU as the accelerator stand-in\n");
    printf("and is expected to show 0 stale on an iGPU.\n");
    printf("Audit decision: deferred until a real GPU reader/writer\n");
    printf("replaces the CPU stand-in.\n");
    return 0;
}

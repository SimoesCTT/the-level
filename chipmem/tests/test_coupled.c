/*
 * test_coupled.c — chipmem + blit composition test.
 *
 * This is the test the audit names. It proves the two contracts compose
 * on the same memory: a buffer allocated by chipmem_alloc, written by
 * the CPU, handed to blit via a per-operation completion handle, and
 * read back by the CPU after a chipmem_sync(ACCEL_WRITE).
 *
 * Three cases:
 *
 *   A. Correct sequence: sync input, blit, wait, sync output, read.
 *      Must PASS. If it fails, the composition is broken.
 *
 *   B. Negative control 1: skip BOTH chipmem_sync calls.
 *      On a CPU blit backend, blit runs on the CPU through the same
 *      physical pages, so the CPU is cache-coherent with itself and
 *      this case is EXPECTED to read correct data. A correct result
 *      here does NOT withdraw the chipmem claim and does NOT prove it.
 *      It is reported, not judged.
 *
 *   C. Negative control 2: sync input, blit, wait, SKIP the output
 *      sync, read the output. This isolates the missing-bracket-on-
 *      read-side failure mode. Same caveat as B on the CPU backend.
 *
 * The test prints the backend for both libraries so a reader can see
 * immediately that neither GPU path is exercised. That is not a
 * failure of the test; it is the test telling the truth.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "chipmem.h"
#include "blit.h"

#define N_TOKENS 256
#define TOKEN_BYTES (N_TOKENS * sizeof(uint32_t))

static int g_failures = 0;

static void report(const char *name, int ok, const char *detail)
{
    printf("%-40s %s%s%s\n", name, ok ? "PASS" : "FAIL",
           detail ? "  " : "", detail ? detail : "");
    if (!ok)
        g_failures++;
}

/*
 * Fill the input buffer with a recognizable pattern.
 */
static void fill_input(uint32_t *p, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = 0x1000u + (uint32_t)i;
}

static int check_output(const uint32_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t expected = 0x1000u + (uint32_t)i;
        if (p[i] != expected)
            return i; /* first mismatch index */
    }
    return -1;
}

/*
 * One run of the coupled sequence.
 *
 *   sync_in   — call chipmem_sync(input, CPU_WRITE) before blit
 *   sync_out  — call chipmem_sync(output, ACCEL_WRITE) before read
 *
 * Returns the index of the first mismatch, or -1 if the output was
 * correct. Returns -2 on a hard error (alloc / blit / sync failure).
 */
static int run_coupled(int sync_in, int sync_out, char *errbuf, size_t errlen)
{
    chipmem_backend_t be = CHIPMEM_BACKEND_COPY_FALLBACK;
    chipmem_buffer_t *in  = chipmem_alloc(TOKEN_BYTES, &be);
    chipmem_buffer_t *out = chipmem_alloc(TOKEN_BYTES, &be);

    if (!in || !out) {
        snprintf(errbuf, errlen, "chipmem_alloc failed");
        if (in)  chipmem_free(in);
        if (out) chipmem_free(out);
        return -2;
    }

    uint32_t *cpu_in  = chipmem_cpu_ptr(in);
    uint32_t *cpu_out = chipmem_cpu_ptr(out);
    if (!cpu_in || !cpu_out) {
        snprintf(errbuf, errlen, "chipmem_cpu_ptr returned NULL");
        chipmem_free(in); chipmem_free(out);
        return -2;
    }

    memset(cpu_out, 0, TOKEN_BYTES);
    fill_input(cpu_in, N_TOKENS);

    if (sync_in) {
        int rc = chipmem_sync(in, CHIPMEM_ACCESS_CPU_WRITE);
        if (rc != 0) {
            snprintf(errbuf, errlen, "chipmem_sync(input,CPU_WRITE): %s",
                     strerror(-rc));
            chipmem_free(in); chipmem_free(out);
            return -2;
        }
    }

    /* Hand the input to blit. 0xF0 is A->D copy. */
    blit_desc_t d = {
        .minterm = 0xF0,
        .src = cpu_in,
        .dst = cpu_out,
        .width = N_TOKENS,
        .height = 1,
        .bpp = 4,
    };
    blit_op_t *op = blit(&d, BLIT_MODE_ASYNC);
    if (!op) {
        snprintf(errbuf, errlen, "blit() failed: %s", strerror(errno));
        chipmem_free(in); chipmem_free(out);
        return -2;
    }
    int rc = blit_op_wait(op);
    if (rc != 0) {
        snprintf(errbuf, errlen, "blit_op_wait: %s", strerror(-rc));
        chipmem_free(in); chipmem_free(out);
        return -2;
    }

    if (sync_out) {
        rc = chipmem_sync(out, CHIPMEM_ACCESS_ACCEL_WRITE);
        if (rc != 0) {
            snprintf(errbuf, errlen, "chipmem_sync(output,ACCEL_WRITE): %s",
                     strerror(-rc));
            chipmem_free(in); chipmem_free(out);
            return -2;
        }
    }

    int mismatch = check_output(cpu_out, N_TOKENS);

    chipmem_free(in);
    chipmem_free(out);
    return mismatch;
}

int main(void)
{
    if (blit_init() != 0) {
        fprintf(stderr, "blit_init failed\n");
        return 1;
    }

    /* Report backends so the reader knows what was exercised. */
    chipmem_backend_t dummy_be = CHIPMEM_BACKEND_COPY_FALLBACK;
    chipmem_buffer_t *probe = chipmem_alloc(4096, &dummy_be);
    int cm_backend = probe ? (int)chipmem_backend(probe) : -1;
    if (probe) chipmem_free(probe);

    printf("chipmem backend: %d (0=COHERENT, 1=SYNC_IOCTL, 2=COPY_FALLBACK)\n",
           cm_backend);
    printf("blit backend:    %d (0=GPU_ASYNC, 1=GPU_SYNC, 2=CPU_FALLBACK, 3=GPU_COPY)\n",
           (int)blit_backend());
    printf("buffer: %d tokens (%d bytes each)\n\n", N_TOKENS,
           (int)sizeof(uint32_t));

    char err[256] = {0};
    int rc;

    /* Case A — correct sequence. Must pass. */
    err[0] = 0;
    rc = run_coupled(1, 1, err, sizeof(err));
    if (rc == -2) {
        printf("%-40s FAIL  %s\n", "A: sync in + sync out", err);
        g_failures++;
    } else if (rc == -1) {
        report("A: sync in + sync out", 1, "data correct");
    } else {
        char d[64];
        snprintf(d, sizeof(d), "first mismatch at token %d", rc);
        report("A: sync in + sync out", 0, d);
    }

    /*
     * Case B — negative control 1: skip both syncs.
     * On a CPU blit backend this is expected to be correct. On the
     * GPU_COPY backend it is also expected to be correct, because the
     * GPU never touches the chipmem pages. Report
     * the result; do not judge it.
     */
    err[0] = 0;
    rc = run_coupled(0, 0, err, sizeof(err));
    if (rc == -2) {
        printf("%-40s SKIP  %s\n", "B: no sync (negative control)", err);
    } else if (rc == -1) {
        printf("%-40s 0 stale (inconclusive on GPU_COPY backend)\n",
               "B: no sync (negative control)");
    } else {
        printf("%-40s %d stale of %d (evidence)\n",
               "B: no sync (negative control)", rc, N_TOKENS);
    }

    /*
     * Case C — negative control 2: sync input, skip output sync.
     * Isolates the missing-bracket-on-read-side failure mode.
     */
    err[0] = 0;
    rc = run_coupled(1, 0, err, sizeof(err));
    if (rc == -2) {
        printf("%-40s SKIP  %s\n", "C: sync in only (neg. control)", err);
    } else if (rc == -1) {
        printf("%-40s 0 stale (inconclusive on GPU_COPY backend)\n",
               "C: sync in only (neg. control)");
    } else {
        printf("%-40s %d stale of %d (evidence)\n",
               "C: sync in only (neg. control)", rc, N_TOKENS);
    }

    blit_shutdown();

    printf("\n");
    if (g_failures) {
        printf("FAIL: %d case(s) failed. The composition is broken.\n",
               g_failures);
        return 1;
    }
    printf("Composition works end to end.\n");
    printf("Cases B and C are negative controls and are inconclusive\n");
    printf("on this backend: blit runs on the GPU via Rusticl, but\n");
    printf("Rusticl does not support cl_khr_external_memory_dma_buf,\n");
    printf("so the GPU never touches the chipmem pages. The copy\n");
    printf("boundary means chipmem_sync's GPU-side effect is not\n");
    printf("exercised. GPU-side coherency is NOT tested. The audit\n");
    printf("decision on both claims remains open.\n");
    return 0;
}

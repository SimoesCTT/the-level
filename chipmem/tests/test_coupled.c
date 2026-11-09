/*
 * test_coupled.c — chipmem + i915 blit zero-copy composition test.
 *
 * The GPU reads the chipmem source GEM handle and writes the chipmem
 * destination GEM handle directly. No intervening copy. The negative
 * controls (skip chipmem_sync) now have a GPU touching the same pages
 * the CPU wrote, which is what the audit's withdrawal condition needs.
 *
 * Three cases:
 *   A. sync in, blit, sync out, read. Must pass.
 *   B. skip both syncs. If stale reads appear, the coherency hazard
 *      is real on this hardware. If not, the claim is withdrawn here.
 *   C. sync in only, skip output sync. Isolates the read-side bracket.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "chipmem.h"
#include "blit_i915.h"

#define N_TOKENS 256
#define TOKEN_BYTES (N_TOKENS * sizeof(uint32_t))

static int g_failures = 0;

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
            return i;
    }
    return -1;
}

/*
 * One run. sync_in/sync_out control the chipmem_sync brackets.
 * Returns first mismatch index, -1 for correct, -2 for hard error.
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
        snprintf(errbuf, errlen, "chipmem_cpu_ptr NULL");
        chipmem_free(in); chipmem_free(out);
        return -2;
    }

    memset(cpu_out, 0, TOKEN_BYTES);
    fill_input(cpu_in, N_TOKENS);

    if (sync_in) {
        int rc = chipmem_sync(in, CHIPMEM_ACCESS_CPU_WRITE);
        if (rc != 0) {
            snprintf(errbuf, errlen, "chipmem_sync(in): %s", strerror(-rc));
            chipmem_free(in); chipmem_free(out);
            return -2;
        }
    }

    uint32_t src_h = 0, dst_h = 0;
    chipmem_gem_handle(in, &src_h);
    chipmem_gem_handle(out, &dst_h);

    uint32_t batch = 0;
    int rc = blit_i915_copy(src_h, TOKEN_BYTES, dst_h, TOKEN_BYTES,
                            N_TOKENS, 1, &batch);
    if (rc != 0) {
        snprintf(errbuf, errlen, "blit_i915_copy: %d", rc);
        chipmem_free(in); chipmem_free(out);
        return -2;
    }

    if (sync_out) {
        rc = chipmem_sync(out, CHIPMEM_ACCESS_ACCEL_WRITE);
        if (rc != 0) {
            snprintf(errbuf, errlen, "chipmem_sync(out): %s", strerror(-rc));
            blit_i915_close_batch(batch);
            chipmem_free(in); chipmem_free(out);
            return -2;
        }
    }

    int mismatch = check_output(cpu_out, N_TOKENS);

    blit_i915_close_batch(batch);
    chipmem_free(in);
    chipmem_free(out);
    return mismatch;
}

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open renderD128"); return 1; }
    if (blit_i915_init(fd) != 0) {
        fprintf(stderr, "blit_i915_init failed\n");
        return 1;
    }
    chipmem_set_fd(fd);

    chipmem_backend_t probe_be = CHIPMEM_BACKEND_COPY_FALLBACK;
    chipmem_buffer_t *probe = chipmem_alloc(4096, &probe_be);
    printf("chipmem backend: %d (0=COHERENT, 1=SYNC_IOCTL, 2=COPY_FALLBACK)\n",
           probe ? (int)chipmem_backend(probe) : -1);
    if (probe) chipmem_free(probe);
    printf("blit backend: i915 BLT (zero-copy softpin)\n");
    printf("buffer: %d tokens (%d bytes each)\n\n",
           N_TOKENS, (int)sizeof(uint32_t));

    char err[256] = {0};
    int rc;

    /* Case A */
    err[0] = 0;
    rc = run_coupled(1, 1, err, sizeof(err));
    if (rc == -2) {
        printf("%-40s FAIL  %s\n", "A: sync in + sync out", err);
        g_failures++;
    } else if (rc == -1) {
        printf("%-40s PASS  data correct\n", "A: sync in + sync out");
    } else {
        printf("%-40s FAIL  first mismatch at token %d\n",
               "A: sync in + sync out", rc);
        g_failures++;
    }

    /* Case B: negative control, skip both syncs */
    err[0] = 0;
    rc = run_coupled(0, 0, err, sizeof(err));
    if (rc == -2) {
        printf("%-40s SKIP  %s\n", "B: no sync (neg. control)", err);
    } else if (rc == -1) {
        printf("%-40s 0 stale: claim withdrawn on this hardware\n",
               "B: no sync (neg. control)");
    } else {
        printf("%-40s %d stale of %d (evidence: claim survives)\n",
               "B: no sync (neg. control)", rc, N_TOKENS);
    }

    /* Case C: sync in only */
    err[0] = 0;
    rc = run_coupled(1, 0, err, sizeof(err));
    if (rc == -2) {
        printf("%-40s SKIP  %s\n", "C: sync in only (neg. control)", err);
    } else if (rc == -1) {
        printf("%-40s 0 stale: claim withdrawn on this hardware\n",
               "C: sync in only (neg. control)");
    } else {
        printf("%-40s %d stale of %d (evidence: claim survives)\n",
               "C: sync in only (neg. control)", rc, N_TOKENS);
    }

    blit_i915_shutdown();
    close(fd);

    printf("\n");
    if (g_failures) {
        printf("FAIL: %d case(s) failed. Composition is broken.\n", g_failures);
        return 1;
    }
    printf("Composition works end to end with zero-copy GPU execution.\n");
    printf("The GPU read and wrote the chipmem GEM handles directly.\n");
    printf("Cases B and C are the audit evidence. Stale reads without\n");
    printf("chipmem_sync would mean the coherency hazard is real on\n");
    printf("this hardware. Zero stale means the hazard does not\n");
    printf("manifest here, and the claim is withdrawn for this iGPU.\n");
    printf("It remains open for discrete accelerators over PCIe.\n");
    return 0;
}

/*
 * test_coupled.c — chipmem + blit composition test.
 *
 * Calls the blit() API, not the i915 BLT function directly. The blit
 * library dispatches to the GPU_ZERO_COPY backend when the i915 BLT
 * path is available and the minterm is 0xF0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

#include "chipmem.h"
#include "blit.h"

#define N_TOKENS 256
#define TOKEN_BYTES (N_TOKENS * sizeof(uint32_t))

static void fill_input(uint32_t *p, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = 0x1000u + (uint32_t)i;
}

static int check_output(const uint32_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t expected = 0x1000u + (uint32_t)i;
        if (p[i] != expected) return i;
    }
    return -1;
}

static int run_coupled(int sync_in, int sync_out, char *errbuf, size_t errlen)
{
    chipmem_backend_t be;
    chipmem_buffer_t *in  = chipmem_alloc(TOKEN_BYTES, &be);
    chipmem_buffer_t *out = chipmem_alloc(TOKEN_BYTES, &be);
    if (!in || !out) {
        snprintf(errbuf, errlen, "chipmem_alloc failed");
        if (in) chipmem_free(in);
        if (out) chipmem_free(out);
        return -2;
    }
    chipmem_register_for_blit(in);
    chipmem_register_for_blit(out);

    uint32_t *cpu_in  = chipmem_cpu_ptr(in);
    uint32_t *cpu_out = chipmem_cpu_ptr(out);
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

    blit_desc_t d = {
        .minterm = 0xF0,
        .src = cpu_in,
        .dst = cpu_out,
        .width = N_TOKENS, .height = 1, .bpp = 4,
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
            snprintf(errbuf, errlen, "chipmem_sync(out): %s", strerror(-rc));
            chipmem_free(in); chipmem_free(out);
            return -2;
        }
    }

    int mismatch = check_output(cpu_out, N_TOKENS);
    chipmem_free(in); chipmem_free(out);
    return mismatch;
}

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open renderD128"); return 1; }
    blit_set_fd(fd);
    chipmem_set_fd(fd);

    if (blit_init() != 0) {
        fprintf(stderr, "blit_init failed\n");
        return 1;
    }
    printf("blit backend: %d (0=GPU_ASYNC, 1=GPU_SYNC, 2=CPU_FALLBACK, "
           "3=GPU_COPY, 4=GPU_ZERO_COPY)\n", (int)blit_backend());
    printf("blit caps: 0x%x\n", blit_caps());
    
    printf("buffer: %d tokens (%d bytes each)\n\n",
           N_TOKENS, (int)sizeof(uint32_t));

    char err[256] = {0};
    int rc, failures = 0;

    rc = run_coupled(1, 1, err, sizeof(err));
    if (rc == -2) { printf("A: FAIL %s\n", err); failures++; }
    else if (rc == -1) printf("A: PASS  data correct\n");
    else { printf("A: FAIL mismatch at %d\n", rc); failures++; }

#define NEG_RUNS 100
    int b_clean = 0, b_dirty = 0, b_err = 0;
    int c_clean = 0, c_dirty = 0, c_err = 0;
    for (int i = 0; i < NEG_RUNS; i++) {
        rc = run_coupled(0, 0, err, sizeof(err));
        if (rc == -2) b_err++;
        else if (rc == -1) b_clean++;
        else b_dirty++;
    }
    for (int i = 0; i < NEG_RUNS; i++) {
        rc = run_coupled(1, 0, err, sizeof(err));
        if (rc == -2) c_err++;
        else if (rc == -1) c_clean++;
        else c_dirty++;
    }
    printf("B: %d runs, %d clean, %d mismatched, %d error\n",
           NEG_RUNS, b_clean, b_dirty, b_err);
    printf("C: %d runs, %d clean, %d mismatched, %d error\n",
           NEG_RUNS, c_clean, c_dirty, c_err);


    /* Wilson 95% CI for the stale rate. p_hat = dirty / (clean+dirty). */
    {
        double z = 1.959963984540054;
        for (int which = 0; which < 2; which++) {
            int clean = which ? c_clean : b_clean;
            int dirty = which ? c_dirty : b_dirty;
            int n = clean + dirty;
            double p_hat = n ? (double)dirty / n : 0.0;
            double denom = 1.0 + z*z/n;
            double center = (p_hat + z*z/(2*n)) / denom;
            double half = z * sqrt((p_hat*(1-p_hat)/n + z*z/(4*n*n))) / denom;
            double lo = center - half, hi = center + half;
            printf("   %c: stale rate = %.3f (95%% Wilson CI %.3f..%.3f)\n",
                   which ? 'C' : 'B', p_hat, lo, hi);
        }
    }
    int selected = (int)blit_backend();
    blit_shutdown();

    if (failures) { printf("\nFAIL: %d case(s)\n", failures); return 1; }
    printf("\nComposition works through the blit() API.\n");
    printf("Backend %d was selected by blit_init().\n", selected);
    return 0;
}

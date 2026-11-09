#ifndef BLIT_H
#define BLIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * blit — a programmable asynchronous 2D engine with stable completion
 * semantics.
 *
 * Thesis: the Amiga Blitter was not a fast memcpy. It was a logic unit
 * (minterms) plus a mask plus a shift plus a fill, running
 * asynchronously from the CPU, with a well-defined completion signal.
 * Linux has fast copies (dumb buffers) and abstract compute (shaders),
 * but no userspace-accessible fixed-function 2D engine with
 * per-operation completion. This header restores the handle, not the
 * hardware.
 *
 * Honest caveat, stated in the audit and repeated here: an
 * implementation over GPU compute or an i915 batch will be SLOWER than
 * a hand-tuned memcpy() for small transfers and slower than a direct
 * DMA for large ones. The value is composability — chaining many small
 * operations with stable completion — not throughput. A benchmark
 * showing blit() slower than memcpy() is expected, not a bug.
 */

typedef enum {
    /*
     * The operation is queued and this call returns immediately. The
     * caller must wait on the returned blit_op_t before reading the
     * destination or reusing the source.
     */
    BLIT_MODE_ASYNC = 0,

    /*
     * The operation is queued and this call blocks until it completes.
     * Equivalent to ASYNC + immediate wait, but may use a cheaper
     * kernel path (e.g. a synchronous batch).
     */
    BLIT_MODE_SYNC = 1
} blit_mode_t;

/*
 * Completion handle for one queued operation. Opaque. Valid until
 * blit_op_wait() returns or blit_op_release() is called.
 *
 * The stability guarantee: waiting on op N completes when operation N
 * has finished, regardless of what operations N+1, N+2, ... are doing.
 * No "fence and pray." No "wait for the queue to drain." This is the
 * property the Amiga Blitter had and that GPU command queues do not
 * expose directly.
 */
typedef struct blit_op blit_op_t;

/*
 * A blit descriptor. The fields map to what the Amiga Blitter exposed:
 *
 *   minterm  — the logic function, in the Amiga's 8-bit minterm form.
 *              The low four bits are A, B, C, D terms; the high four
 *              bits are the inverted forms. 0xCA is the classic
 *              "masked sprite" minterm, 0xF0 is A->D (straight copy).
 *   mask     — optional per-pixel mask applied before the minterm.
 *              NULL means no mask.
 *   shift    — barrel shift applied to the source before the minterm.
 *              0 means no shift.
 *   src      — source buffer (may be NULL for fill operations).
 *   dst      — destination buffer.
 *   width    — operation width in pixels.
 *   height   — operation height in rows.
 *   src_stride — bytes per source row. 0 means width * bpp.
 *   dst_stride — bytes per destination row. 0 means width * bpp.
 *   bpp      — bytes per pixel: 1, 2, or 4.
 */
typedef struct {
    uint8_t        minterm;
    const uint8_t *mask;
    uint8_t        shift;
    const void    *src;
    void          *dst;
    uint32_t       width;
    uint32_t       height;
    uint32_t       src_stride;
    uint32_t       dst_stride;
    uint8_t        bpp;
} blit_desc_t;

/*
 * Initialize the blit engine. Must be called before any other blit_*
 * function. Returns 0 on success, negative errno on failure.
 *
 * Backend selection is automatic: it prefers a GPU compute or i915
 * batch path where available and falls back to a CPU implementation
 * (which is correct but synchronous and slow) otherwise. The chosen
 * backend is reported by blit_backend().
 */
/*
 * Use an existing DRM fd for the GPU_ZERO_COPY backend. Call before
 * blit_init() if the caller already has a render node open and wants
 * the zero-copy backend to share its GEM handle namespace. If not
 * called, blit_init() opens its own render node.
 */
void blit_set_fd(int fd);

int blit_init(void);

typedef enum {
    /* GPU-backed, asynchronous. The intended path. */
    BLIT_BACKEND_GPU_ASYNC = 0,

    /* GPU-backed but serialized (no real async queue). */
    BLIT_BACKEND_GPU_SYNC = 1,

    /* CPU fallback. Correct, synchronous, slow. */
    BLIT_BACKEND_CPU_FALLBACK = 2,

    /*
     * GPU execution with a copy at the API boundary. The kernel runs on
     * the GPU and the completion handle is a real event, but the source
     * and destination are copied to/from GPU-owned buffers because the
     * backend cannot import the caller's memory zero-copy. This is the
     * honest label for a Rusticl backend on hardware where
     * cl_khr_external_memory_dma_buf is unavailable.
     */
    BLIT_BACKEND_GPU_COPY = 3,

    /*
     * GPU execution with no copy at the API boundary. The kernel reads
     * and writes the caller's buffers directly. The backend must be
     * able to import the caller's memory (a chipmem GEM handle on
     * i915) and submit a fixed-function 2D command stream against it.
     * This is the honest label for the i915 BLT backend on Skylake.
     */
    BLIT_BACKEND_GPU_ZERO_COPY = 4
} blit_backend_t;

/* Report the backend chosen by blit_init(). */
blit_backend_t blit_backend(void);

/*
 * Capability bits reported by blit_caps(). A caller can decide how to
 * structure its pipeline without hardcoding backend names.
 */
#define BLIT_CAP_ZERO_COPY       (1u << 0)  /* GPU touches caller memory */
#define BLIT_CAP_MIN_0_F0        (1u << 1)  /* copy supported */
#define BLIT_CAP_MIN_0_CA        (1u << 2)  /* masked sprite supported */
#define BLIT_CAP_MIN_0_00        (1u << 3)  /* fill supported */
#define BLIT_CAP_ASYNC           (1u << 4)  /* real async completion */

/*
 * Report the capability bitmask of the selected backend. Call after
 * blit_init(). Returns 0 if no backend is selected.
 */
uint32_t blit_caps(void);


/*
 * Queue one 2D operation.
 *
 * On success, returns a completion handle and sets *out_op to it. The
 * caller owns the handle and must call blit_op_wait() or
 * blit_op_release() on it.
 *
 * On failure, returns NULL and sets errno.
 *
 * In BLIT_MODE_SYNC, the operation is complete when this returns and
 * the handle is already satisfied; the caller may still call
 * blit_op_wait() to release it.
 */
blit_op_t *blit(const blit_desc_t *desc, blit_mode_t mode);

/*
 * Block until the given operation completes.
 *
 * Returns 0 on success, negative errno on failure (including if the
 * operation itself failed on the GPU). The handle is consumed: after
 * this returns, the handle is invalid and must not be used again.
 *
 * Waiting on op N does NOT wait for later operations. That is the
 * stability guarantee.
 */
int blit_op_wait(blit_op_t *op);

/*
 * Non-blocking completion check. Returns 1 if complete, 0 if not yet
 * complete, negative errno on failure. Does not consume the handle.
 */
int blit_op_done(blit_op_t *op);

/*
 * Release a handle without waiting. If the operation is still in
 * flight, it continues in the background but the handle is freed; the
 * caller must not reuse the destination buffer until the operation
 * finishes, which the caller can only know by having waited on some
 * later operation that is ordered after this one. In practice, do not
 * use this unless you are discarding the whole queue.
 */
void blit_op_release(blit_op_t *op);

/*
 * Tear down the blit engine. Waits for all outstanding operations and
 * frees backend resources. After this, blit_init() may be called again.
 */
void blit_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* BLIT_H */

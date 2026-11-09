# blit backends

A blit backend is a fixed set of operations behind the blit.h
contract. blit_init() selects one backend at startup; blit(),
blit_op_wait(), and blit_op_done() dispatch to it. The caller never
names a backend. blit_backend() reports which one is active.

## The interface

    struct blit_backend_ops {
        const char *name;
        int  (*init)(int fd);
        void (*shutdown)(void);
        int  (*submit)(const blit_desc_t *desc, blit_mode_t mode,
                       void **out_handle);
        int  (*wait)(void *handle);
        int  (*done)(void *handle);
        void (*release)(void *handle);
    };

A backend exports one `const struct blit_backend_ops *blit_backend_NAME(void)`
and is registered in blit.c's candidate list. blit_init() tries each
in order and keeps the first whose init() returns 0.

## The three backends today

### GPU_ZERO_COPY (i915 BLT) - blit_i915.c

- Hardware: Intel Gen8/Gen9 iGPU (verified on Skylake-U).
- Minterms: 0xF0 only. 0xCA and 0x00 fall through to the CPU.
- Zero-copy: yes. The GPU reads and writes the caller's GEM handles.
- Completion: submit returns after the kernel accepts the batch. The
  real synchronization point is chipmem_sync(dst, ACCEL_WRITE).
  wait is a no-op.
- Requires: buffers registered with chipmem_register_for_blit(), and
  blit_set_fd() with the same render node as chipmem_set_fd().

### GPU_COPY (Rusticl / iris) - blit_cl.c

- Hardware: any GPU with a Rusticl platform.
- Minterms: 0xF0, 0xCA, 0x00.
- Zero-copy: no. Data is copied to and from Rusticl-owned buffers
  because Rusticl does not support cl_khr_external_memory_dma_buf.
- Completion: a real cl_event per operation.
- Requires: RUSTICL_ENABLE=iris in the environment.

### CPU_FALLBACK - blit_cpu.c

- Hardware: any.
- Minterms: 0xF0, 0xCA, 0x00.
- Zero-copy: n/a.
- Completion: synchronous. The handle is complete when submit returns.

## Per-minterm fallback

A backend may return -ENOTSUP from submit for a minterm it cannot
express. blit() then falls through to the CPU backend for that
operation only. This is why 0xCA works on Rusticl and on the CPU,
but not on the i915 BLT backend.

## Adding a discrete backend

1. A way to import the caller's memory zero-copy. NVIDIA: open kernel
   driver with DMA-BUF import. AMD: amdgpu plus a command stream
   (SDMA or GFX).
2. A command stream that performs a 2D copy against the imported
   handles.
3. A fence for completion. The wait op must block on it.

When that backend exists, the discrete chipmem coherency test becomes
meaningful: the GPU touches the chipmem pages, and the negative
controls can produce stale reads if the hazard is real. Until then
the chipmem coherency claim remains open for discrete hardware.

# The Level

**Zero-copy CPU/GPU shared memory for Linux, with a programmable asynchronous 2D engine built on top.**

Most AI systems assume the CPU and the GPU occupy separate address spaces, and that moving data between them is the application's responsibility. This assumption is why GPU acceleration is commonly associated with discrete hardware. On platforms where the GPU shares physical memory with the CPU — including integrated Intel GPUs and a growing number of discrete devices — the assumption does not hold. The hardware can already read and write the same bytes. What was missing was a small, explicit contract for using that capability safely.

This library provides that contract, in two headers:

- `chipmem.h` — allocates a buffer accessible to both the CPU and the GPU, with a single explicit handoff call and a single free.
- `blit.h` — a 2D engine operating on those buffers: copy, masked sprite, and fill, with a per-operation completion handle.

The objective is not to outperform a discrete GPU. The objective is to make the GPU already present in a standard laptop usable for GPU-side work without an intervening copy.

## Verified on integrated hardware

The implementation was built and verified on an Intel Skylake-U platform (HD Graphics 520, `i915` driver, kernel 7.2.13), a 2015-generation integrated GPU. The GPU reads and writes the same physical pages the CPU writes. The composition test performs the following sequence: the CPU fills a buffer, `chipmem_sync` hands it off, `blit` copies it on the GPU, `blit_op_wait` blocks on completion, `chipmem_sync` brackets the read, and the CPU verifies the result. The result is 0 mismatches out of 1024 elements, with no copy at the API boundary.

The handoff is not decorative. The negative control performs the same copy with `chipmem_sync` omitted, repeated 100 times per case:

- Both sync calls omitted: 85 and 99 stale reads out of 100, across two batches.
- Output sync omitted only: 97 and 100 stale reads out of 100.

Without the handoff, the CPU reads the buffer before the GPU has completed, in the majority of runs. The synchronization call is what makes the result correct, and the measurement establishes it.

## Origin

The project began as an Amiga emulator frontend (AmigaDroid). Work on that project produced a design note, "What the Amiga Had That Was Lost," and the conclusion that the honest interpretation of "reinvent it on Linux" is not to reimplement the Amiga hardware, but to identify which capabilities are genuinely absent on Linux today and to re-instantiate the programming model.

The Amiga's distinguishing characteristic was not any individual chip. It was that display, 2D blitting, memory shared with accelerators, and scheduling were exposed to the programmer at the same level of abstraction, with the same simplicity, on the same machine. A 1985 system provided this. A Linux laptop does not, by default. This library implements the portion of that model that proved buildable: the shared-memory contract and the 2D engine.

## API

```c
chipmem_buffer_t *in  = chipmem_alloc(4096, NULL);
chipmem_buffer_t *out = chipmem_alloc(4096, NULL);
chipmem_register_for_blit(in);
chipmem_register_for_blit(out);

uint32_t *src = chipmem_cpu_ptr(in);
uint32_t *dst = chipmem_cpu_ptr(out);
fill(src);

chipmem_sync(in, CHIPMEM_ACCESS_CPU_WRITE);

blit_desc_t d = { .minterm = 0xF0, .src = src, .dst = dst,
                  .width = 1024, .height = 1, .bpp = 4 };
blit_op_t *op = blit(&d, BLIT_MODE_ASYNC);
blit_op_wait(op);

chipmem_sync(out, CHIPMEM_ACCESS_ACCEL_WRITE);
check(dst);
```

`chipmem_alloc` returns a buffer. `chipmem_sync` is the handoff: call it with `CPU_WRITE` after CPU writes, and with `ACCEL_WRITE` after GPU writes. `blit` submits an operation and returns a handle. `blit_op_wait` blocks until completion. The API does not require `begin_cpu_access` / `end_cpu_access` pairs, manual DMA-BUF import, or explicit fence management.

## Relevance to AI workloads

AI pipelines contain substantial non-tensor 2D work: image resizing and normalization, tokenizer layout transformations, attention mask construction, and KV-cache tiling. This work either executes on the CPU, which is serial and comparatively slow, or is offloaded to a GPU with copy-in and copy-out at the boundary, which presumes a GPU with separate memory and the budget to acquire it.

On a platform that shares memory, neither is necessary. The data remains in place and the GPU operates on it directly. This library enables that on hardware already present in the machine. It is not a faster memory copy; it is the contract that makes the integrated GPU usable for this class of work.

## Implementation of the zero-copy path

The GPU does not copy at the API boundary. The path consists of three steps:

1. `chipmem_alloc` creates a GEM object with `DRM_IOCTL_I915_GEM_CREATE`, exports it as a DMA-BUF with `DRM_IOCTL_PRIME_HANDLE_TO_FD`, and maps it for CPU access with `mmap`.
2. `blit` constructs an `XY_SRC_COPY_BLT` command stream, writes it into a batch GEM object, and submits it to the BLT ring with `DRM_IOCTL_I915_GEM_EXECBUFFER2`. The source, destination, and batch are soft-pinned at fixed GPU addresses, without relocations.
3. `chipmem_sync` on the destination brackets the CPU read with `DMA_BUF_IOCTL_SYNC`, which blocks until the GPU has completed.

The complete path, including the details that proved necessary (`MI_BATCH_BUFFER_END` must be `0x05000000`; `GEM_SET_DOMAIN` is retired on this kernel; relocations are rejected on Gen9), is documented in `docs/linux-equivalents.md`.

## Backends

`blit` provides three backends behind a single contract. `blit_init()` selects the most capable available backend, and `blit_caps()` reports its capabilities:

| Backend | Zero-copy | Minterms | Completion |
|---|---|---|---|
| `i915` BLT | yes | `0xF0` | `chipmem_sync` on dst |
| `rusticl` | no (copies) | `0xF0`, `0xCA`, `0x00` | `cl_event` |
| `cpu` | n/a | all three | synchronous |

The `i915` backend performs the operation directly on the `chipmem` buffers. The `rusticl` backend executes kernels on any GPU with a Rusticl platform, but copies at the boundary. The `cpu` backend is always available and serves as the fallback.

A new backend requires one source file and one entry in the `blit_init` candidate list. The interface is documented in `docs/backends.md`.

## Build and test

Requires `libdrm`, `opencl-headers`, `OpenCL-ICD-Loader-devel`, and, for the Rusticl backend, `RUSTICL_ENABLE=iris`.

```bash
cd blit    && make clean && make check
cd chipmem && make clean && make check && make check-coupling
```

`make check` runs the `blit` tests and reports the selected backend. `make check-coupling` runs the composition test: a `chipmem` buffer passes through `blit()`, the GPU reads and writes the same pages, and the CPU reads the result. The output includes the selected backend, the capability mask, and the stale-rate range for the negative controls.

## Scope

The library is complete for the hardware on which it was built and verified: Intel Gen8/Gen9 iGPU with `i915` and an available render node, verified on Skylake-U, kernel 7.2.13.

It also functions, with copying, on any GPU with a Rusticl platform.

Beyond that hardware, the work is an extension: a discrete-GPU zero-copy backend (NVIDIA open kernel driver with DMA-BUF, or `amdgpu`) is specified in `docs/backends.md`. It is not required for the library to be complete, and it cannot be built or tested on the current machine.

`copper` (a scanline-synchronized display agent) was evaluated and found not reachable from userspace on `i915`. It is documented, not implemented.

## What this is not

- It is not a reimplementation of Amiga hardware. The hardware is not relevant; the programming model is, and the Amiga is the origin of the idea.
- It is not an AI accelerator in the sense of additional floating-point throughput. The claim concerns the data path: no copies, one explicit handoff, on the GPU already present.
- It is not a claim that Linux lacks GPU APIs. The claim is narrower: there is no small userspace contract for CPU/accelerator shared memory with a measured coherency guarantee, and no programmable asynchronous 2D engine with per-operation completion behind a single contract.

## Further documentation

- `docs/linux-equivalents.md` — the audit: every claim, the closest existing Linux mechanism, why it does not count, and the condition under which the claim would be withdrawn. Includes the closed paths (udmabuf, `SET_DOMAIN`, `GEM_WAIT`, relocations, Kaggle) and the sampling lesson that changed a withdrawal into a proof.
- `docs/backends.md` — the backend interface, the three backends, and the requirements for a discrete backend.

## License

MIT or Apache-2.0 for `chipmem` and `blit`.

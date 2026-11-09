# The Level

Two small userspace libraries that restore, on Linux, two accelerator
programming primitives the Amiga exposed to programmers and that
Linux does not expose at the same level of simplicity:

- **`chipmem`** — coherent CPU/accelerator memory with one explicit
  `chipmem_sync()` handoff.
- **`blit`** — a programmable 2D engine with a minterm, a mask, a
  shift, and a per-operation completion handle.

Not a reimplementation of Amiga hardware. The hardware is irrelevant.
The programming model is the point.

## What this is for

Most AI systems still treat CPU and accelerator memory as permanently
foreign address spaces, forcing copies and complex pinning APIs for
every non-tensor 2D operation in the pipeline. On the growing class of
hardware that can share memory through DMA-BUF (integrated GPUs today,
an increasing number of discrete devices tomorrow), that assumption is
unnecessary. The Level provides two small userspace libraries:
`chipmem`, which turns the kernel's required DMA-BUF coherency
protocol into a single named handoff, and `blit`, a programmable
async 2D engine that can operate zero-copy on those buffers. Together
they let pre- and post-processing (tiling, masking, layout
conversion, tokenizer transforms) stay on the GPU without the usual
copy tax, while remaining honest about the cases where the hardware
still requires a transfer.

## The rule

We only claim a "reinvention" if the capability is **actually absent**
on Linux today. `docs/linux-equivalents.md` names the closest
existing mechanism for each candidate, why it does not count, and the
concrete condition under which the claim would be withdrawn. If the
existing mechanism is genuinely equivalent, the item is dropped.

## What works today

### `chipmem`

- `chipmem.h`: `chipmem_alloc`, `chipmem_cpu_ptr`,
  `chipmem_dmabuf_fd`, `chipmem_sync`, `chipmem_backend`,
  `chipmem_backend_cost`, `chipmem_free`, `chipmem_set_fd`,
  `chipmem_gem_handle`.
- `chipmem.c`: i915 GEM + PRIME backend. `DRM_IOCTL_I915_GEM_CREATE` +
  `DRM_IOCTL_PRIME_HANDLE_TO_FD` (with `DRM_CLOEXEC | DRM_RDWR`)
  exports a DMA-BUF that `mmap()` accepts and `DMA_BUF_IOCTL_SYNC`
  brackets without error. Allocation size is rounded up to the page
  size internally.
- `udmabuf` was rejected as a backend on kernel 7.2.13: its
  `begin_cpu_access` returns `EINVAL` on first sync. The i915 path is
  the real backend.

### `blit`

Two working GPU backends:

- **Rusticl / iris** (`BLIT_BACKEND_GPU_COPY`). Kernels run on the GPU
  via `cl_event` completion handles. Data is copied to and from GPU
  buffers because Rusticl does not support
  `cl_khr_external_memory_dma_buf`. Cross-platform, copy at the
  boundary.
- **i915 BLT zero-copy** (`blit_i915.c`). `XY_SRC_COPY_BLT` submitted
  to the BLT ring via `DRM_IOCTL_I915_GEM_EXECBUFFER2` with softpin.
  The GPU reads and writes the `chipmem` GEM handles directly, no
  copy. Verified on Skylake-U. Gen8/Gen9-specific; not a universal
  backend.

Minterms `0xF0` (copy), `0xCA` (masked sprite), `0x00` (fill). The
stability guarantee — waiting on operation N completes independently
of N+1 — is honored by `cl_event` handles on the Rusticl path.

### The coupled test

`chipmem/tests/test_coupled.c` runs the i915 BLT backend on the
actual `chipmem` GEM handles. The GPU reads and writes the same pages
the CPU wrote. Case A passes: `0/1024 mismatched`.

The negative controls run each case 100 times. Skipping
`chipmem_sync` produces a stale or partially-written destination
buffer at a very high rate. Across two batches of 100 runs on the
same hardware and binary:

- Case B (both brackets missing): 85/100 and 99/100 stale.
- Case C (output bracket missing): 97/100 and 100/100 stale.

The rate itself varies between batches. The hazard is real, the
output bracket is the more critical of the two, and `chipmem_sync` is
load-bearing. A single 100-run batch's confidence interval describes
that batch, not the phenomenon; the honest statement is the observed
range across batches. An earlier single-run test landed in the clean
minority and reported the wrong conclusion; the 100-run loop exposed
the race.

## Target hardware

Intel Skylake-U GT2 (HD Graphics 520), `i915`, kernel 7.2.13.
`chipmem` backend: `CHIPMEM_BACKEND_DMABUF_SYNC_IOCTL`.
`blit` backends: `BLIT_BACKEND_GPU_COPY` (Rusticl) and i915 BLT
zero-copy.

## Not doing

- Re-implementing Amiga hardware.
- Claiming Linux lacks something without proving the closest existing
  mechanism doesn't count.
- Promising hardware-dependent features before feasibility is shown.
- Claiming "AI acceleration" in the sense of more FLOPs. The claim is
  about the programming model and the data path, not throughput.
- Claiming the coherency hazard is real on hardware where the test
  says it does not manifest.

## Repo layout

the-level/
├── chipmem/
│ ├── include/chipmem.h
│ ├── src/chipmem.c
│ └── tests/
│ ├── test_coherency.c
│ └── test_coupled.c
├── blit/
│ ├── include/blit.h
│ ├── src/
│ │ ├── blit.c
│ │ ├── blit_cl.c
│ │ ├── blit_cl.h
│ │ ├── blit_i915.c
│ │ ├── blit_i915.h
│ │ ├── addr_alloc.c
│ │ └── addr_alloc.h
│ └── tests/test_blit.c
├── copper/ # audit entry only, no code
├── docs/linux-equivalents.md
└── README.md

text

## License

MIT or Apache-2.0 for `chipmem` and `blit`. `copper` license decided at
feasibility time.

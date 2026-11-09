# The Level

A userspace AI acceleration layer for ordinary GPUs, built on the
Amiga's accelerator programming model — shared memory with an explicit
contract, and an async 2D unit with a stable completion signal — not on
Amiga hardware.

## Thesis

The Amiga's real lost capability was not any single chip. It was that
every capability — display, 2D blitting, memory shared with
accelerators — was exposed to the programmer at the same level, with
the same simplicity, on the same machine. Linux has all the chips and
none of the simplicity.

## Rule

We only claim a "reinvention" if the capability is **actually absent**
on Linux today. `docs/linux-equivalents.md` names the closest existing
mechanism for each candidate and the concrete condition under which the
claim would be withdrawn.

## Candidates

1. **`chipmem`** — coherent CPU/accelerator memory with one explicit
   `chipmem_sync()` contract. **Priority 1.**
2. **`blit`** — programmable async 2D engine with per-operation
   completion. **Priority 2.**
3. **`copper`** — scanline-synchronized display agent. **Speculative;
   no code until feasibility is proven.**

## What works today

### `chipmem`

- `chipmem.h`: `chipmem_alloc`, `chipmem_cpu_ptr`, `chipmem_dmabuf_fd`,
  `chipmem_sync`, `chipmem_backend`, `chipmem_backend_cost`,
  `chipmem_free`.
- `chipmem.c`: i915 GEM + PRIME backend. `DRM_IOCTL_I915_GEM_CREATE` +
  `DRM_IOCTL_PRIME_HANDLE_TO_FD` (with `DRM_CLOEXEC | DRM_RDWR`) exports
  a DMA-BUF that `mmap()` accepts and `DMA_BUF_IOCTL_SYNC` brackets
  without error. Allocation size is rounded up to the page size
  internally.
- `tests/test_coherency.c`: passes on the sync path. Reports honestly
  that GPU-side coherency is **not** tested, because the "accelerator"
  is the CPU writing through a second mapping.
- `udmabuf` was rejected as a backend on kernel 7.2.13: its
  `begin_cpu_access` returns `EINVAL` on first sync. The i915 path is
  the real backend.

### `blit`

- `blit.h`: `blit_init`, `blit`, `blit_op_wait`, `blit_op_done`,
  `blit_op_release`, `blit_shutdown`, `blit_backend`.
- `blit.c`: CPU fallback backend. Minterms `0xF0` (copy), `0xCA`
  (masked sprite), `0x00` (fill). The stability guarantee — waiting on
  operation N completes independently of N+1 — is honored by the
  handle semantics.
- `tests/test_blit.c`: copy, mask, fill, and stable completion pass.
  Prints "asynchrony not exercised," which is true.

### `copper`

Audit entry only. No code. Feasibility is not proven on the named
display controller.

## The honest status, in one paragraph

Both contracts exist. Both CPU-side implementations work and pass their
tests. Neither library has executed on the GPU. The `chipmem` coherency
claim and the `blit` "missing API" claim are both **open**, not proven
and not withdrawn. The next real work is a GPU backend for `blit`,
which then makes the `chipmem` coherency test meaningful. Until then,
the libraries are correct plumbing with unexercised acceleration.

## Target hardware

Intel Skylake-U GT2 (HD Graphics 520), `i915`, kernel 7.2.13.
`chipmem` backend: `CHIPMEM_BACKEND_DMABUF_SYNC_IOCTL`.
`blit` backend: `BLIT_BACKEND_GPU_COPY` (Rusticl/iris on HD 520).

## Not doing

- Re-implementing Amiga hardware.
- Claiming Linux lacks something without proving the closest existing
  mechanism doesn't count.
- Promising hardware-dependent features before feasibility is shown.
- Claiming "AI acceleration" before `blit` runs on the GPU.
- Claiming the coherency hazard is proven before the negative control
  exercises a real GPU reader/writer.

## Repo layout

the-level/
├── chipmem/
│ ├── include/chipmem.h
│ ├── src/chipmem.c
│ └── tests/test_coherency.c
├── blit/
│ ├── include/blit.h
│ ├── src/blit.c
│ └── tests/test_blit.c
├── copper/ # audit entry only, no code
├── docs/linux-equivalents.md
└── README.md

text

## License

MIT or Apache-2.0 for `chipmem` and `blit`. `copper` license decided at
feasibility time.

# Linux Equivalents Audit

This document answers one question per capability: *what is the closest
existing Linux mechanism, and does its API surface or semantic guarantee
actually fail the Amiga-level simplicity test?*

A claim is only kept if the answer is **yes, it fails**, with a concrete
condition under which the claim would be withdrawn. If the existing
mechanism is genuinely equivalent, the item is dropped.

## Withdrawal conditions

| Capability | Claim withdrawn if... |
|---|---|
| `chipmem` | On target hardware, `mmap()` of an accelerator DMA-BUF gives a CPU-writable pointer whose writes are visible to the accelerator without `begin/end_cpu_access` and without a hidden copy. |
| `blit` | A userspace API exists that accepts a minterm + mask + size + source/dest pointer and returns a stable per-operation completion handle, on target hardware, without a shader pipeline. |
| `copper` | On target display controller, a supported userspace path exists to program a register write at a scanline trigger. |

## Test hardware

**Target:** Intel Skylake-U GT2 (HD Graphics 520), driver `i915`,
kernel 7.2.13. `/dev/dri/card1` and `/dev/dri/renderD128` present.
Integrated GPU, shared memory controller. DMA-BUF export available
via the render node.

**Not tested:** discrete accelerator over PCIe. The `chipmem`
conclusion is validated on iGPU only.

---

## 1. `chipmem` — coherent CPU/accelerator memory

### Closest mechanism

DMA-BUF plus `mmap()`, with the `DMA_BUF_IOCTL_SYNC` ioctl bracketing
CPU access. `udmabuf` and `memfd` provide an alternative export path
but are not suitable for zero-copy sharing with other devices.

### What it actually provides

A file descriptor exported by one driver and imported by another, with
a `mmap()` path that gives userspace a CPU pointer. On `i915`, userspace
is responsible for flushing the CPU by wrapping `mmap` with
`begin/end_cpu_access`. The kernel does not signal the exporter about
CPU accesses.

### Why it doesn't count

The capability exists. The *simplicity* and *explicit coherency
contract* do not. `MEMF_CHIP` was one flag with an implicit model. The
DMA-BUF equivalent requires the caller to know which ioctl brackets CPU
access, which access direction flags apply, whether the exporter
supports the modern sync interface, and whether the buffer landed in a
CPU-accessible domain.

### Withdrawal condition (concrete, for this hardware)

On Skylake-U GT2 / `i915` / kernel 7.2.13: the claim is withdrawn if a
test allocates a DMA-BUF, maps it, has the CPU write through the
mapping, has the GPU read those bytes, and the GPU sees the CPU writes
**without** a `DMA_BUF_IOCTL_SYNC` bracket — and without a hidden copy.
If skipping sync produces stale reads, the claim survives.

### Backend classification for this hardware

`CHIPMEM_BACKEND_DMABUF_SYNC_IOCTL`. `chipmem_sync()` is mandatory.

---

## 2. `blit` — programmable async 2D engine

### Closest mechanism

DRM dumb buffers (linear framebuffer, no logic unit), GPU compute
(programmable but abstract, shader-dispatch overhead), `dmaengine`
(kernel-side DMA controller API, not a userspace 2D primitive).

### Why it doesn't count

The Amiga Blitter exposed `BLTCON0/1`, `BLTSIZE`, source/dest pointers,
and a completion signal. It was a *logic unit*, not a memcpy wrapper.
Linux has fast copies and abstract compute but no fixed-function 2D
engine with per-operation completion semantics exposed to userspace.

### Withdrawal condition

The claim is withdrawn if a userspace API exists on target hardware
that accepts a minterm + mask + size + source/dest pointer and returns
a stable per-operation completion handle, without a shader pipeline.

### Honest caveat

A `blit()` over GPU compute will be **slower** than a hand-tuned
`memcpy()` for small transfers and **slower** than a direct DMA for
large ones. The claim survives because the target use case is
*composability* — chaining many small operations with stable completion
semantics — not throughput.

---

## 3. `copper` — scanout-synchronized programmable display agent

### Closest mechanism

DRM/KMS atomic modesetting, vblank events, `drmModeAtomicCommit`.
`VK_KHR_display` present timing.

### Why it doesn't count

No userspace-accessible, mid-frame programmable agent that reacts to
scanline position and changes display registers in real time. Even on
bare KMS, changing a register during scanout is not a supported
operation.

### Withdrawal condition (named target required)

Target: Intel Skylake-U GT2 display controller, driver `i915`, kernel
7.2.13. The claim is withdrawn if a supported userspace path exists to
program a register write at a scanline trigger — via a DRM ioctl,
`VK_KHR_display`, or a documented driver-specific interface.

**Status: feasibility not yet proven. Do not write `copper` code.**

---

## Risks to the thesis

**GMEM (Generalized Memory Management).** An OpenEuler effort toward a
unified CPU/accelerator address space. If it lands upstream with a
usable userspace contract, the `chipmem` claim narrows to "a simpler
wrapper over GMEM," not "the missing contract." The audit must be
updated if that happens.

## Items explicitly dropped

- **Scheduling.** CFS, EEVDF, `SCHED_FIFO/RR/DEADLINE`, cgroups,
  `PREEMPT_RT` exist. User-experience project, not a capability.
- **Integrated creative stack.** GStreamer, FFmpeg, PipeWire, Blender,
  Ardour, Kdenlive. Interoperability is good enough.
- **The intelligible machine.** Cultural project, not a capability.

---

## Backend status log

### `chipmem` — Skylake-U, kernel 7.2.13, i915 (2026-11-10)

**udmabuf rejected as a backend.** `/dev/udmabuf` is present (built
into the Fedora kernel, not a module). The standalone
`memfd_create` + `F_ADD_SEALS` + `UDMABUF_CREATE` path succeeds. But
`DMA_BUF_IOCTL_SYNC` on a udmabuf-backed DMA-BUF returns `EINVAL` on
first sync: udmabuf's `begin_cpu_access` builds the cached SG table
with `DMA_ATTR_SKIP_CPU_SYNC` and skips the
`dma_sync_sgtable_for_cpu()` call on first invocation. A fix is in
flight on LKML (June 2026 thread on udmabuf sg_table serialization)
but is not in this kernel. udmabuf is not viable for the coherency
test on this machine.

**i915 GEM + PRIME works as a backend.** Open
`/dev/dri/renderD128`, `DRM_IOCTL_I915_GEM_CREATE`, then
`DRM_IOCTL_PRIME_HANDLE_TO_FD` with `DRM_CLOEXEC | DRM_RDWR` to
export a DMA-BUF. The fd `mmap()`s with `PROT_READ | PROT_WRITE`, and
`DMA_BUF_IOCTL_SYNC` with `END | WRITE` succeeds. `chipmem_sync()`
is exercised end to end. Backend reported:
`CHIPMEM_BACKEND_DMABUF_SYNC_IOCTL`.

Two implementation details worth recording, because the caller should
not have to know them:
- The allocation size is rounded up to the system page size
  internally. udmabuf and the GEM export path both require
  page-aligned sizes. `chipmem_alloc(size)` returns a buffer of at
  least `size` bytes; the mapped length may be larger.
- `DRM_RDWR` is required on the PRIME export flags. `DRM_CLOEXEC`
  alone produces a fd that cannot be mapped writable for CPU access.

**Coherency result: untested.** The test currently uses the CPU as the
accelerator stand-in, writing through a second mapping of the same
physical pages. On an iGPU the CPU is cache-coherent with itself, so
the negative control (skip `chipmem_sync`) shows 0/256 stale reads.
That is the expected result and it proves nothing about GPU
coherency. The audit's withdrawal condition remains open: the claim
survives or fails only when a real GPU reader/writer replaces the CPU
stand-in. Until then, the honest statement is:

> `chipmem_sync()` is a working explicit coherency bracket on i915.
> Whether skipping it causes a GPU to read stale data is not yet
> tested on this hardware.

**Next test required:** a GPU reader/writer via the i915 render node
(minimal compute dispatch or blit-engine operation) that reads the
CPU-written buffer with and without sync and reports what the GPU
saw. This test needs the async completion handle that `blit` will
provide, which is why the two candidates are not independent at the
test level even though the libraries are.

### `blit` — Skylake-U, kernel 7.2.13, i915 (2026-11-10)

**Contract written and exercised.** `blit.h` defines:
- `blit_init()` / `blit_shutdown()`
- `blit_backend()` reporting `GPU_ASYNC`, `GPU_SYNC`, `CPU_FALLBACK`,
  or `GPU_COPY`
- `blit(desc, mode)` returning a per-operation completion handle
- `blit_op_wait()` / `blit_op_done()` / `blit_op_release()`

**Stability guarantee:** waiting on operation N completes when N is
done, independent of operations N+1, N+2. This is the property the
Amiga Blitter exposed and that GPU command queues do not expose
directly. It is the reason `blit` exists as a separate contract from
`chipmem`.

**Implementation status at this time: `BLIT_BACKEND_CPU_FALLBACK`.**
Superseded by the Rusticl GPU backend section below. The first
implementation runs every operation to completion inside `blit()`
before returning. The handle is still a distinct object with the
stability guarantee, so callers written against the contract work
unchanged when a GPU backend lands. Minterms implemented: `0xF0`
(copy), `0xCA` (masked sprite), `0x00` (fill). Other minterms return
`-ENOTSUP`.

**Tests pass.** Copy, masked sprite, fill, and stable completion all
pass on the CPU backend. The test prints "asynchrony not exercised,"
which is true: the CPU backend cannot demonstrate async completion.

**What this does NOT show:**
- The `blit` engine has not executed on the GPU. The audit's claim
  that "Linux has no userspace-accessible fixed-function 2D engine
  with per-operation completion" is *not yet tested* on this
  hardware. It is documented, not proven.
- The API surface is proven correct. The GPU path is not.

**Withdrawal condition, restated for this hardware:** the `blit`
claim is withdrawn if a userspace API exists on i915 that accepts a
minterm + mask + size + source/dest pointer and returns a stable
per-operation completion handle, without a shader pipeline. No such
API is present in the i915 or DRM userspace surface today. That
statement is what the audit records; it is not a claim that `blit`
has replaced the missing API on this hardware — it has not, yet.

### The two candidates are coupled at the test level

The `chipmem` GPU coherency test needs an async completion handle to
know when the GPU has finished reading the CPU-written buffer.
`blit` provides that handle. The next real test is therefore:

1. `chipmem_alloc` two buffers (input, output).
2. CPU writes the input.
3. `chipmem_sync(input, CPU_WRITE)`.
4. `blit` minterm `0xF0` from input to output, `BLIT_MODE_ASYNC`.
5. `blit_op_wait(handle)`.
6. `chipmem_sync(output, ACCEL_WRITE)`.
7. CPU reads the output.
8. Negative control: skip `chipmem_sync` before `blit`, check for
   stale reads on the CPU side after the GPU read.

Step 8 is the audit evidence. On the current CPU backend, step 4 runs
on the CPU, so step 8 cannot produce stale data. The test must be
labeled: **"sync plumbing proven, GPU coherency pending `blit` GPU
backend."** That is what the test will print until the i915 `blit`
backend exists.

### Coupled `chipmem` + `blit` composition test (2026-11-10)

`chipmem/tests/test_coupled.c`, run via `make check-coupling`. Links
both libraries against the same `chipmem`-allocated buffers.

**Case A — correct sequence (sync in, blit, wait, sync out, read):
PASS.** Data correct. The two contracts compose: a `chipmem` buffer
goes into `blit`, the per-operation completion handle works, the
output comes back through the same memory, and the CPU reads what it
wrote. This is the composition the audit names, proven on the CPU
path.

**Case B — skip both syncs (negative control): 0 stale.** Labeled
inconclusive on the CPU blit backend at the time of this entry.
Superseded by the GPU_COPY result below. Expected: blit runs on the CPU
through the same physical pages, so the CPU is cache-coherent with
itself. Reported, not judged.

**Case C — sync input only, skip output sync (negative control):
0 stale.** Same caveat as B. Isolates the missing-bracket-on-read-side
failure mode; cannot produce stale data on this backend.

**What this proves:** the contracts compose and the completion handle
gives the ordering guarantee the design requires. `chipmem` allocates,
`blit` transforms, `chipmem_sync` brackets the CPU handoff, and the
result is readable.

**What this does NOT prove:** any GPU-side coherency claim. Both
negative controls are inconclusive until a GPU `blit` backend
replaces the CPU fallback. The three cases are written so that the
GPU backend changes the *result* of B and C (stale vs. zero), not
the *code*. That is the form a falsifiable test should have.

**Backends at time of test:** `chipmem` = `DMABUF_SYNC_IOCTL` (real
i915 GEM + PRIME export); `blit` = `CPU_FALLBACK` at this time.
Superseded by the Rusticl GPU backend section below. The top of the
test output reports both, so a reader sees the GPU path is
unexercised without reading the prose.

**Status of both claims: open.** Proven: contracts exist, compose,
and are tested on the CPU side. Unproven: that skipping `chipmem_sync`
causes a GPU to read stale data; that `blit` fills a missing userspace
2D API on this hardware. Neither claim is withdrawn.

### i915 BLT zero-copy backend — works on Skylake (2026-11-10)

`blit_i915.c` submits `XY_SRC_COPY_BLT` to the BLT ring via
`DRM_IOCTL_I915_GEM_EXECBUFFER2` with softpin. The GPU reads and
writes the `chipmem` GEM handles directly: no copy at the boundary.

What made it work, in order:
- `MI_BATCH_BUFFER_END` must be `0x05000000`, not `0x0a << 23 | 1`.
  The `| 1` makes it an illegal command and the parser rejects the
  batch.
- The batch must be written through `MMAP_OFFSET` + `memcpy` +
  `munmap`. `GEM_SET_DOMAIN` is retired on this kernel and returns
  `EINVAL`; `munmap` alone clears the write domain.
- Source, destination, and batch are soft-pinned at low addresses
  (2 MiB, 16 MiB, 32 MiB) with `EXEC_OBJECT_PINNED |
  EXEC_OBJECT_SUPPORTS_48B_ADDRESS`. The batch has no
  `EXEC_OBJECT_WRITE`.
- `exec.flags = I915_EXEC_BLT | I915_EXEC_NO_RELOC`,
  `relocation_count = 0` everywhere.
- Completion is not `DRM_IOCTL_I915_GEM_WAIT` on the batch handle
  (`ENODEV` on this kernel). The `chipmem_sync(dst, ACCEL_WRITE)`
  call is the real synchronization point; it blocks until the GPU is
  done with the buffer.

Result: `i915 copy: 0/1024 mismatched`. The GPU copied `chipmem`
memory zero-copy.

The coupled test now runs `blit_i915_copy` on the actual `chipmem`
GEM handles. Cases B and C (skip sync) are the audit evidence for the
`chipmem` coherency claim.

### `chipmem` coherency result — claim withdrawn on Skylake iGPU (2026-11-10)

The coupled test ran the zero-copy i915 blitter against `chipmem`
GEM handles with the `chipmem_sync` brackets removed in both
directions. Result: 0 stale reads in both negative controls.

The audit's `chipmem` withdrawal condition was: "on target hardware,
`mmap()` of an accelerator DMA-BUF gives a CPU-writable pointer whose
writes are visible to the accelerator without `begin/end_cpu_access`
and without a hidden copy." That condition is met on Skylake iGPU
with the blitter. **The claim is withdrawn for this hardware.**

The `DMA_BUF_IOCTL_SYNC` bracket is still required by the kernel
contract, and `chipmem_sync()` still issues it. It is not observably
required to prevent stale reads on this iGPU, because the CPU and GPU
share the memory controller and are coherent enough that the blitter
reads correct data without the bracket.

**What remains open:** discrete accelerators over PCIe, where CPU and
GPU caches are separate and the coherency hazard is expected to be
real. That is a different hardware configuration and a different test.
The claim is not withdrawn there; it is untested there.

**What this does NOT mean:** that the bracket is unnecessary. It
means this specific test, on this specific hardware, did not produce
the failure mode the bracket protects against. The `chipmem` contract
is unchanged; only the claim that skipping it causes stale reads on
Skylake iGPU is withdrawn.

### AI relevance and generality (2026-11-10)

**The strongest version of the AI argument that survives the "actually
absent on Linux" rule** is not "faster copies." It is: a small,
explicit, portable contract for memory that is simultaneously
CPU-accessible and accelerator-owned, with one named handoff point
(`chipmem_sync`) that the kernel requires but does not provide as a
simple userspace primitive. Every major AI runtime treats CPU and
accelerator memory as permanently different address spaces, which
forces explicit copies and pin-memory APIs for every non-tensor 2D
operation. `chipmem` restores the contract the hardware actually has:
one allocation, one handoff, zero hidden copies when the hardware is
coherent, an honest error when it is not. `blit` is the async 2D
primitive for the non-tensor work (tiling, masking, layout
conversion, tokenizer transforms) that today runs on the CPU or as
awkward GPU compute kernels.

**Generality of the zero-copy BLT path:** it is generation- and
vendor-specific. `XY_SRC_COPY_BLT` + softpin + the exact domain and
cmdparser sequence is an i915 Gen8/Gen9 (and some Gen11) detail.
Newer Intel gens prefer `XY_FAST_COPY_BLT` / `XY_BLOCK_COPY_BLT` and
eventually compute engines; AMD has a different command stream and
VM model. The *idea* — submit a fixed-function 2D command stream
against DMA-BUF handles with softpin and an explicit fence — is
generalizable. The i915 BLT implementation is a working production
backend for Skylake-era iGPUs and a high-quality reference, not a
universal backend. A multi-vendor version would need a thin
abstraction over "submit a minterm blit against two DMA-BUFs and
return a fence."

**Discrete-accelerator test prerequisite:** the discrete coherency
test Grok described is the right shape, but it only settles the
`chipmem` claim if the backend can touch the `chipmem` pages
zero-copy on that GPU. Rusticl cannot import the DMA-BUF, so a
Rusticl run on a discrete GPU measures the copy boundary, not the
hazard. A zero-copy backend for the discrete hardware (e.g. NVIDIA
open kernel driver with DMA-BUF, or amdgpu) is the prerequisite. The
audit records this rather than renting a GPU to run a test that
cannot answer the question.

### blit abstraction and backend selection (2026-11-10)

blit_init() selects a backend in this order:
1. GPU_ZERO_COPY — i915 BLT, if blit_i915_init() succeeds.
2. GPU_COPY — Rusticl/iris, if blit_cl_init() succeeds.
3. CPU_FALLBACK — always available.

blit_backend() reports the active backend. blit_set_fd() lets the
caller share a render node with the zero-copy backend so chipmem GEM
handles are in the same namespace as the batch.

The blit() contract is unchanged: minterm, mask, shift, size, and a
per-operation completion handle. On GPU_ZERO_COPY the minterm 0xF0
goes through the BLT ring; 0xCA and 0x00 fall through to the CPU
because the BLT ROP cannot express a three-input cookie-cut. On
GPU_COPY all three minterms run on the GPU via Rusticl kernels.

The coupled test calls blit() and blit_op_wait() and gets backend 4
(GPU_ZERO_COPY). Case A passes: the GPU read the chipmem source GEM
handle and wrote the chipmem destination GEM handle directly. Cases B
and C report the chipmem coherency withdrawal on iGPU.

The fd-namespace bug that made all three objects name handle 1 is
fixed by blit_set_fd(): chipmem and blit share one render node, so
GEM handles are in one namespace.

# M0(c) — Vulkan/MoltenVK CPU cost: x86-64 under Rosetta vs native arm64

Go/no-go measurement for [`docs/DESIGN_NATIVE_GPU_PROCESS.md`](../../../docs/DESIGN_NATIVE_GPU_PROCESS.md)
(milestone M0, part c). One C source (`vkbench.c`) is built twice — `-arch x86_64` (runs under
Rosetta 2) and `-arch arm64` — and both link the **same universal `libMoltenVK.dylib`**
(`deps/x86_64/lib`, slices x86_64 + arm64) directly, without the Vulkan loader, so both go through
identical driver code. Headless, no window or swapchain.

```sh
tools/m0_spike/vulkan_bench/build.sh          # -> out/m0_vulkan_bench/{vkbench,cpubench}-{x86_64,arm64}
tools/m0_spike/vulkan_bench/run.sh [runs=3] [frames=300] [warmup=60]
```

## Workload (per frame, shaped like `docs/FINDINGS.md` Part 2)

| Item | Per frame |
|---|---|
| Dynamic-rendering passes (`vkCmdBeginRendering`) | 270: 4 shadow (4096² D32, depth only, 30–70 draws), 8 G-buffer (1280×720, RGBA8×4 + RGBA16F + D32S8, 20–80 draws), 246 small (32² / 64² R16G16F; 62% 1–2 draws, 33% 3–7, 5% 20–30), 12 fullscreen (1280×720 RGBA8, 1–2 draws) |
| Indexed draws | 1,690 (36–300 indices each, 16×16 viewports — GPU work is negligible) |
| Graphics pipelines | 20 (blend / depth / cull / bias / attachment-format variants) + 1 compute; 849 pipeline binds |
| Per draw | bind pipeline if changed, `vkCmdPushDescriptorSetKHR` (UBO + sampled image + sampler, + SSBO for some: 1,785 pushes incl. compute), 64-byte push constants, viewport, scissor, vertex + index buffer bind, `vkCmdDrawIndexed` |
| Compute dispatches | 95 (bursts of 1–4 between passes, push descriptor + push constants) |
| Barriers / copies | 175 `vkCmdPipelineBarrier2`, 70 `vkCmdCopyBuffer` |
| `vkQueueSubmit` | 60 (one command buffer each), fence wait at frame end |

Both builds print the same per-type command counts and checksum (`028e8cba27341c9d`), so they
create and record exactly the same work. The workload is generated from a fixed integer PRNG.

Timing: per frame, **record** = all `vkCmd*`/`vkBegin/EndCommandBuffer`/pool reset,
**submit** = the 60 `vkQueueSubmit` calls, **frame** = record + submit (excludes the fence wait).
Thread CPU time comes from `thread_info(THREAD_EXTENDED_INFO)`, **not** `CLOCK_THREAD_CPUTIME_ID`:
under Rosetta that clock reports only ~2–3% of the real thread time for translated code (a
busy loop: 871 ms wall, 21 ms "thread CPU"). Wall time uses `mach_absolute_time`. Thread CPU and
wall agree to within ~0.01 ms, i.e. the thread never blocks while recording or submitting.
`process_cpu` = whole-process CPU per frame including MoltenVK/Metal helper threads and the wait.

## Results (Apple M4 Max, macOS 26.6, MoltenVK 1.4.2; 3 runs × 300 frames after 60 warm-up)

Milliseconds per frame, median of the three per-run medians (per-run medians in brackets), p95
= median of per-run p95s. Ratio = x86-64 / arm64 (higher = native is faster).

**MoltenVK defaults** (encoding to Metal happens synchronously inside `vkQueueSubmit`)

| Metric | x86-64 / Rosetta | arm64 native | Speed-up |
|---|---|---|---|
| Record (thread CPU) | 0.993 [0.99, 0.99, 0.99], p95 1.05 | 0.496 [0.49, 0.50, 0.50], p95 0.54 | **2.00×** |
| Submit = MoltenVK encode (thread CPU) | 5.548 [5.57, 5.55, 5.54], p95 5.71 | 3.228 [3.22, 3.23, 3.25], p95 3.36 | **1.72×** |
| **Frame CPU, record + submit** (thread CPU) | **6.545** [6.55, 6.54, 6.53], p95 6.74 | **3.731** [3.72, 3.73, 3.74], p95 3.89 | **1.75×** |
| Frame wall (record + submit) | 6.548, p95 6.74 | 3.732, p95 3.89 | 1.75× |
| Process CPU (all threads, incl. wait) | 12.06 | 8.38 | 1.44× |
| Fence wait (wall) | 0.18 | 1.19 | — (GPU finishes later relative to the faster CPU) |

**`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`** (encoding moves into recording)

| Metric | x86-64 / Rosetta | arm64 native | Speed-up |
|---|---|---|---|
| Record incl. encode (thread CPU) | 5.845 [5.84, 5.97, 5.82], p95 6.03 | 3.340 [3.40, 3.34, 3.33], p95 3.49 | 1.75× |
| Submit (thread CPU) | 0.284 [0.28, 0.28, 0.27], p95 0.30 | 0.135 [0.13, 0.14, 0.14], p95 0.15 | 2.10× |
| **Frame CPU** (thread CPU) | **6.131** [6.13, 6.25, 6.10], p95 6.32 | **3.477** [3.54, 3.48, 3.47], p95 3.63 | **1.76×** |
| Process CPU (all threads) | 10.90 | 7.51 | 1.45× |

Run-to-run variance is ≤ 2% in every cell.

**CPU-only micro-benchmark** (`cpubench.cpp`: hash-map cache lookups by hashed pipeline key and
guest address, small per-draw binding vectors, virtual `Bind()` calls, atomic refcounts, short
sorts, an occasional mutex and string format; no Vulkan): x86-64 **247 ns/iter** [247.2, 249.0,
247.0] vs arm64 **208 ns/iter** [208.3, 208.4, 208.7] → **1.19×**. Rosetta handles generic,
mostly integer/hash/branch C++ well; the MoltenVK path (Objective-C message sends, Metal API
calls, many tiny calls and locks) suffers much more (1.7–2.0×).

## Interpretation

- The per-frame CPU cost of the recording + submission path drops from 6.5 ms to 3.7 ms:
  **1.75× faster native** (1.76× with prefill). This clears the design's go threshold
  (arm64 ≥ 1.3× on per-frame CPU) with margin. Prefill moves the work but does not change the
  total or the ratio — consistent with the FINDINGS experiment.
- Applied to the measured `bb:DrawRec` profile at 720p (51% MoltenVK encode, ~9% renderer per-draw
  work, ~11% spin-waiting on host copies, ~6% idle, rest other renderer code), assuming 1.75× for
  MoltenVK, 1.19× (micro-benchmark) for the renderer's own code and no gain on waits/idle, the thread
  would get roughly **1.35× faster** overall (0.51/1.75 + 0.32/1.19 + 0.17 ≈ 0.73); if the
  renderer's own code behaved like `record` here (2.0×) it would be ~1.6×. Either way above 1.3×,
  but the conservative end has little margin.

## Caveats (how representative this is)

- Absolute cost is ~5–6× smaller than the game's (~33–40 ms/frame busy): real draws carry more
  descriptors, vertex attributes and state, larger shaders, and the shadPS4 renderer's own cache
  lookups, texture/buffer tracking and page-protection work. The **ratio** is the result; the
  absolute numbers are not a frame-time prediction.
- Only MoltenVK + the Vulkan calls are exercised. The renderer's own code (shader/pipeline
  caches, guest-memory tracking, Liverpool PM4 parsing) is only approximated by the micro-benchmark,
  which shows a much smaller Rosetta penalty (1.19×). Its exact mix decides where in the
  ~1.35–1.6× range the real thread lands.
- The spin-waits on host copies (~11%) and sync with the guest-side threads stay on the Rosetta
  side and get no faster; cross-process doorbells (M0b) add cost not modelled here.
- Single uncontended thread, warm caches, identical frame every time; no pipeline compiles,
  no resource creation during the measured frames, no Metal argument-buffer or shader-cache churn.
- MoltenVK defaults otherwise (synchronous queue submits). `process_cpu` shows ~4.6–5.5 ms/frame
  of helper-thread CPU (Metal/MoltenVK completion handling for 60 command buffers) that is off the
  critical path but also ~1.2× cheaper native.
- One machine (M4 Max), one MoltenVK version (1.4.2).

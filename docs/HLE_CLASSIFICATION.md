# HLE classification for the GPU-process split (M2)

Companion to `docs/DESIGN_NATIVE_GPU_PROCESS.md` §3.3. Classifies every HLE function the GPU
library (`gpu/`) registers, for the split into `bb-probe` (game + runtime, x86-64 under Rosetta)
and `bbgpu-host` (renderer, native arm64), sharing guest memory at identical addresses.

Analysis only, from the working tree on top of commit `0ffadd5` (2026-10-11). No code was changed and the
game was not run.

## 1. Method and the 327 count

- Registration: `LIB_FUNCTION(nid, lib, ver, mod, fn)` (`gpu/shim/core/libraries/libs.h:16`)
  calls `SymbolsResolver::AddSymbol`, which `gpu/shim/bbgpu.cpp` implements by appending to
  `g_symbols`. `bbgpu_symbol_count()` is `g_symbols.size()`, printed by `src/probe.c:773`.
- Four registration functions run (`bbgpu_register_kernel` at `src/probe.c:648`, then
  `bbgpu_init` at `src/probe.c:772`):

  | Registrar | File | `LIB_FUNCTION` lines | Registered |
  |---|---|---:|---:|
  | `Libraries::GnmDriver::RegisterLib` | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2920` | 254 | 254 |
  | `Libraries::VideoOut::RegisterLib` | `gpu/shadps4/core/libraries/videoout/video_out.cpp:467` | 30 | 30 |
  | `Libraries::Kernel::RegisterEventQueue` | `gpu/shadps4/core/libraries/kernel/equeue.cpp:651` | 17 | 17 |
  | `Libraries::AvPlayer::RegisterLib` | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:253` | 27 | **26** (`sceAvPlayerPrintf` is commented out) |
  | **Total** | | 328 | **327** ✔ |

- The 327 registrations are 324 distinct NIDs and 321 distinct C++ functions:
  `Fwvh++m9IQI` (sceGnmGetGpuCoreClockFrequency), `R3TYO7Tdv5o` and `ut57TBmRQN0` are registered
  twice under different library names (`bbgpu_resolve` matches on the NID only, so the second
  copy is never returned). VideoOut registers `sceVideoOutGetResolutionStatus`,
  `sceVideoOutAddVblankEvent` and `sceVideoOutGetVblankStatus` under two NIDs each.
- **Resolution order:** `runtime_resolve` (`src/runtime.c:180-232`) tries every runtime
  resolver first and calls `bbgpu_resolve` last. None of the 327 NIDs appears in `src/*.c`, so
  none is shadowed by the runtime.
- **What Bloodborne actually imports:** each NID was matched against the eboot's import table
  (`out/macdata/out/analysis.json` `imports`, 686 entries) and the linker's list of imports left to
  the host (`out/macdata/out/link.json` `unresolved_imports`, which also covers the bundled
  modules). Both give the same answer: **77 of the 327 are imported**. The other 250 are never
  called by CUSA03173. They are classified anyway, but only the 77 matter for M2. In the tables,
  **BB = Y** marks the imported ones.

## 2. Class legend

| Class | Meaning | Where it runs after the split |
|---|---|---|
| **B** | Command builder or pure helper: writes PM4/data only into caller-supplied guest memory (or out-params), no renderer state | Compiled into `bb-probe`; no IPC |
| **B\*** | Builder that embeds a **host address** (VideoOut label array, embedded shader code) | `bb-probe`, after that address is published to it (see §6) |
| **S** | Submit/flip: hands work to the GPU | Proxy: ring record + doorbell |
| **Q** | Query of GPU/VideoOut state | Shared status block (or a constant) |
| **E** | Event queues, GPU/VideoOut events, event-struct helpers | Runtime side (`bb-probe`); the host posts events |
| **R** | Resource/registration: compute queues, VideoOut open/close/buffers/modes | Message to the host (load-time, mostly) |
| **A** | AvPlayer | `bb-probe` (see §6, P8) |
| **N** | Stub or "not available in retail firmware": returns a constant, touches nothing | `bb-probe`, as is |

## 3. Summary: counts per class per library

Each cell is *registrations (imported by Bloodborne)*.

| Library (registration) | B | B* | S | Q | E | R | A | N | Total |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| GnmDriver | 46 (18) | 3 (1) | 7 (4) | 5 (2) | 3 (3) | 3 (3) | – | 187 (19) | **254** (50) |
| VideoOut | 3 (1) | – | 1 (1) | 8 (1) | 9 (1) | 8 (4) | – | 1 (0) | **30** (8) |
| Kernel equeue | – | – | – | – | 17 (3) | – | – | – | **17** (3) |
| AvPlayer | – | – | – | – | – | – | 23 (16) | 3 (0) | **26** (16) |
| **All** | **49** (19) | **3** (1) | **8** (5) | **13** (3) | **29** (7) | **11** (7) | **23** (16) | **191** (19) | **327** (77) |

### 3.1 The 77 imported functions by class (the actual M2 work list)

| Class | n | Functions |
|---|---:|---|
| B | 19 | `sceGnmSet{Cs,Es,Gs,Hs,Ls,Ps,Vs}Shader`, `sceGnmUpdate{Gs,Hs,Ps,Vs}Shader`, `sceGnmInsert{Push,PushColor,Pop,Set}Marker`, `sceGnmDrawInitDefaultHardwareState200`, `sceGnmDispatchInitDefaultHardwareState`, `sceGnmLogicalCuMaskToPhysicalCuMask`, `sceVideoOutSetBufferAttribute` |
| B\* | 1 | `sceGnmInsertWaitFlipDone` |
| S | 5 | `sceGnmSubmitCommandBuffers`, `sceGnmSubmitAndFlipCommandBuffers`, `sceGnmSubmitDone`, `sceGnmDingDong`, `sceVideoOutSubmitFlip` |
| Q | 3 | `sceGnmAreSubmitsAllowed`, `sceGnmGetTheTessellationFactorRingBufferBaseAddress` (constant), `sceVideoOutGetFlipStatus` |
| E | 7 | `sceKernelCreateEqueue`, `sceKernelDeleteEqueue`, `sceKernelWaitEqueue`, `sceGnmAddEqEvent`, `sceGnmDeleteEqEvent`, `sceGnmGetEqEventType`, `sceVideoOutAddFlipEvent` |
| R | 7 | `sceGnmMapComputeQueue`, `sceGnmMapComputeQueueWithPriority`, `sceGnmUnmapComputeQueue`, `sceVideoOutOpen`, `sceVideoOutClose`, `sceVideoOutRegisterBuffers`, `sceVideoOutSetFlipRate` |
| A | 16 | `sceAvPlayer{Init,PostInit,AddSource,Start,Stop,Pause,Resume,Close,SetLooping,EnableStream,GetVideoDataEx,GetAudioData,GetStreamInfo,StreamCount,CurrentTime,IsActive}` |
| N | 19 | `sceGnmDebugHardwareStatus`, `sceGnmDisableMipStatsReport`, `sceGnmSetupMipStatsReport`, `sceGnmRequestMipStatsReportAndReset`, `sceGnmDriverCaptureInProgress`, `sceGnmDriverTriggerCapture`, `sceGnmFlushGarlic`, `sceGnmGetEqTimeStamp`, `sceGnmInsertSetColorMarker`, `sceGnmIsUserPaEnabled`, `sceGnmLogicalCuIndexToPhysicalCuIndex`, `sceGnmRequestFlipAndSubmitDone`, `sceGnmSetGsRingSizes`, `sceGnmValidateCommandBuffers`, `sceRazor{IsLoaded,CaptureImmediate,CaptureSinceLastFlip,CaptureCommandBuffersOnlyImmediate,CaptureCommandBuffersOnlySinceLastFlip}` |

Only **12 imported functions need any cross-process traffic** (5 S + 7 R), plus one shared
status read (`sceVideoOutGetFlipStatus`) and one shared word (`sceGnmAreSubmitsAllowed`). Everything
else runs in `bb-probe` with no IPC.

## 4. Findings that affect the design

1. **The split surface is much smaller than "327".** 77 are imported, 19 of those are stubs. The
   design's "classify 327" (M2) is really "proxy 12 functions, share ~2 status structs, move
   equeue + AvPlayer + 20 builders into `bb-probe`".
2. **Draws do not go through HLE.** `sceGnmDrawIndex*`, `sceGnmDispatchDirect` etc. are not
   imported: Bloodborne's statically linked libGnm writes draw packets inline. The per-draw HLE
   traffic is the shader binds (`sceGnmSet*Shader` / `sceGnmUpdate*Shader`), which are pure
   builders. So the hottest HLE calls need no IPC, as the design hoped.
3. **The hot S-path is `sceGnmSubmitCommandBuffers`: ~65–80 calls per frame, ~5000/s**
   (`docs/parallel_gpu.md` §Measurements). It must be a fire-and-forget ring push + doorbell; it
   returns `ORBIS_OK` unless validation fails, and the validation is pure, so no reply is needed.
   A Mach-message round trip per submit (tens of µs × 70) would cost 1–3 ms/frame and must be
   avoided.
4. **No hot-path function needs a synchronous answer from the host**, provided three pieces of
   host state are mirrored in shared memory (all solvable, see §6):
   `submission_lock` (SubmitDone/WaitGpuIdle), the flip-queue depth (queue-full errors of
   `SubmitFlip`/`SubmitAndFlip`), and the VideoOut label address (`InsertWaitFlipDone`).
5. **The host never needs to know about event queues.** Today the host threads call into the
   kernel equeue (`IrqC` handlers from `sceGnmAddEqEvent`, `VideoOutDriver::Flip`'s
   `flip_events` loop, the PresentThread's `vblank_events` loop). After the split the host
   simply posts *every* GfxEop / Compute*RelMem / GpuIdle interrupt, every flip (with
   `flip_arg`) and every vblank (with its count) to the event ring; a dispatcher thread in
   `bb-probe` triggers whichever queues registered. `sceGnmAddEqEvent`,
   `sceVideoOutAdd{Flip,Vblank}Event` then become game-local with no message at all.
6. **Equeue and AvPlayer move wholesale into `bb-probe`** as an x86 C++ "HLE-lite" library
   (`equeue.cpp`, the `bb:kernel_service` boost::asio thread from `bbgpu.cpp`, `avplayer/*`,
   FFmpeg x86 from `deps/x86_64`). AvPlayer cannot live in the host: it calls guest function
   pointers (allocators, file and event callbacks) on its own threads
   (`runtime_thread_attach_host`), and uses `runtime_file_translate`. This settles the design's
   open M6 question: AvPlayer stays in `bb-probe`.
7. **Draw-preparation `Build(dcb)` currently runs on the guest's submitting thread**
   (`Liverpool::SubmitGfx`, `gpu/shadps4/video_core/amdgpu/liverpool.cpp:1763`). After the split
   it runs on a host thread, so the game thread gets cheaper submits (a small extra win not
   counted in the design).
8. **Timebase is a real cross-process dependency** (not mentioned in the design): the host
   writes guest-visible times (`FlipStatus.tsc/process_time/submit_tsc`, `VblankStatus`, and
   PM4 EOP timestamps via `sceKernelReadTsc`/`sceKernelGetTscFrequency` in
   `video_core/amdgpu/pm4_cmds.h:349`). In-process these are Rosetta `rdtsc` and
   `runtime_process_time_us`; in a native arm64 host `Common::FencedRDTSC` reads `cntvct_el0`,
   which need not match Rosetta's `rdtsc`, and `runtime_*` time functions do not exist. See P6.

## 5. Builder dependencies on GPU-library state

Checked every `sceGnm*` builder body (`gnmdriver.cpp:176-1856`, `2393-2581`, `2861-2875`)
for references to `liverpool`, `presenter`, `VideoOut::`, `sdk_version`, `submission_lock`,
`send_init_packet`, `frames_submitted`, `asc_next_offs_dw`, `tessellation_factors_ring_addr`,
`EmulatorSettings`, `Memory::Instance`, `IrqC`, `GetEqueue`, `DebugState`:

| Dependency | Used by | Mirror needed on the game side? |
|---|---|---|
| `liverpool` pointer | **no builder** (only S/R functions) | No |
| `sceKernelIsNeoMode()` | Dispatch/Draw*InitDefaultHardwareState*, DrawIndex*, Set/ResetVgtControl, InitToDefaultContextState* | Constant 0 today (`bbgpu.cpp`); `bb-probe` must provide the same constant |
| `sdk_version` (gnmdriver static, set in `RegisterLib` from `sceKernelGetCompiledSdkVersion`) | `sceGnmDrawInitToDefaultContextStateInternal{Command,Size}` (not imported); `PerformSubmit` (host) | `bb-probe` already has the SDK version (it passes it in `BbGpuConfig`); just set it |
| Init/context sequences (`gnmdriver_init.h`), `UseNeoCompatSequences`, `indirect_sgpr_offsets` | builders | `constexpr` data, compiled in; nothing to mirror |
| `tessellation_factors_ring_addr` | `sceGnmGetTheTessellationFactorRingBufferBaseAddress` | Constant `0x7F0000000` from `gpu/shim/core/address_space.h:23-24`; both sides compile the same constant |
| VideoOut `port->buffer_labels` address | `sceGnmInsertWaitFlipDone` (**imported**), `PatchFlipRequest` | **Yes** — publish once (P3) |
| Static embedded-shader code arrays (library image) | `sceGnmSetEmbedded{Ps,Vs}Shader` (not imported) | Yes if ever used (P9) |
| `LOG_*`, `ASSERT*`, `HLE_TRACE`, `DebugState` | all | Need the logging/assert shim in `bb-probe`; `HLE_TRACE` and `DebugState` are already empty stubs in `gpu/shim` |
| Headers | `pm4_cmds.h` pulls `gnmdriver.h` → `kernel/equeue.h` (boost::asio) and `kernel/time.h` | Build-only: fine once equeue is in `bb-probe` too |

Conclusion: the builders are clean. Apart from the label address, nothing in GnmDriver needs to
be mirrored for them, and none uses x86-specific code (the `rdtsc` asm is only reached from
VideoOut/PM4 timestamp code on the host).

## 6. Problem cases and suggested handling

| # | Case | Why it doesn't fit | Suggested handling |
|---|---|---|---|
| P1 | `sceGnmSubmitDone` (imported, 1/frame) | Reads `liverpool->IsGpuIdle()` **synchronously** to decide whether to set `submission_lock`; later submits/DingDongs block in `WaitGpuIdle` until the host's GpuIdle IRQ (`ResetSubmissionLock`). The guest relies on "idle ⇒ all fence labels written" (heap-corruption fix in `docs/parallel_gpu.md`, 2026-10-01). | Put `submission_lock` in the shared block. SubmitDone: wait until 0, set to 1, push a SUBMIT_DONE record, return. Host: when the SubmitDone is processed and `IsGpuIdle()` (num_submits 0 and work retired), store 0 with release and wake (`os_sync_wake_by_address_all`), then post GpuIdle. Cost: at most one host turnaround before the *next* submit, once per frame; no round trip inside SubmitDone. `sceGnmAreSubmitsAllowed` reads the same word (stale OK). |
| P2 | `sceGnmSubmitAndFlipCommandBuffers` / `sceVideoOutSubmitFlip` (imported, 1/frame) | Return `0x80d11081` / `ORBIS_VIDEO_OUT_ERROR_FLIP_QUEUE_FULL` when `flip_pending_num > 16`, a host value under `port_mutex`. `SubmitFlip` also validates `buffer_slots[idx]` (host). | Make `flip_pending_num` and `gc_queue_num` shared atomics: `bb-probe` checks and increments (and stamps `submit_tsc`) before pushing the record; the host decrements in `Flip()`. Keep a game-side copy of the registered slot map (filled from `RegisterBuffers` replies) for validation. No reply needed. |
| P3 | `sceGnmInsertWaitFlipDone` (imported, B\*), `PatchFlipRequest`, `sceVideoOutGetBufferLabelAddress` | Embed the address of `VideoOutPort::buffer_labels`, a host heap array. Only the host dereferences it (Liverpool's `WaitRegMem`/`WriteData`, `IsVoLabel` range check at `liverpool.cpp:1298`). | Either (a) publish the host's label address once in the static part of the shared block — `bb-probe` only embeds the number, never dereferences it — or (b) place `buffer_labels` in the shared block at an identical address. (a) is enough today; (b) is safer if anything on the game side ever reads labels. |
| P4 | `sceGnmSubmitCommandBuffers` (imported, ~70/frame) | Hot. Its pointer/size arrays are guest memory the game may reuse right after return. | Copy the arrays into a variable-length ring record (count is small); validate game-side; no reply. The DCB/CCB contents themselves are read asynchronously by the host, as today (PS4 semantics keep them alive until the GPU consumes them). Requires release on push / acquire on pop (Rosetta TSO vs arm64). |
| P5 | `sceGnmMapComputeQueue(+WithPriority)`, `sceVideoOutRegisterBuffers`, `sceVideoOutOpen` (imported, load-time) | Return host-allocated ids (vqid from `asc_queues` SlotVector, buffer group index, port handle). | Synchronous control request/reply — load-time only, a few calls. (Alternatively allocate the ids game-side, but not worth it.) `MapComputeQueue` also writes `*read_ptr_addr = 0` into guest memory: do that game-side or host-side, either works with shared memory. |
| P6 | Times written by the host (`FlipStatus.tsc`, `.process_time`, `.submit_tsc`, `VblankStatus`, PM4 EOP timestamps) | Use `sceKernelReadTsc` (`Common::FencedRDTSC`: x86 `rdtsc` under Rosetta vs `cntvct_el0` natively), `sceKernelGetTscFrequency` and `sceKernelGetProcessTime` (`runtime_*` functions in `bb-probe`). Host-side timing + x86-specific code. | Give the host a timebase at launch: `process_start` (CLOCK_MONOTONIC is system-wide on macOS) and a calibrated (guest TSC, CLOCK_MONOTONIC ns) pair + guest TSC Hz measured by `bb-probe`. The host converts its own CLOCK_MONOTONIC to guest TSC. Verify early whether Rosetta's `rdtsc` simply equals `cntvct_el0` (then no conversion is needed) — needs a measurement. |
| P7 | Host threads trigger kernel equeues (`IrqC` handlers from `sceGnmAddEqEvent`, `VideoOutDriver::Flip`, PresentThread vblank, `sceVideoOutSubmitEopFlip` one-shot) | Host would call into `bb-probe` state. | Event ring host → game: `IRQ{id}`, `FLIP{flip_arg, index}`, `VBLANK{count}`. A `bb-probe` dispatcher thread triggers the queues. The EOP-flip one-shot registration stays host-side (the host handles the SubmitAndFlip record, registers the one-shot, then submits — ordering preserved). Side fix: `kqueues` (`equeue.cpp:29`) is an unsynchronised `unordered_map` already accessed from several threads; the dispatcher adds another — add a lock. |
| P8 | AvPlayer (16 imported) | Calls guest callbacks on its own threads; uses `runtime_file_translate`; calls `Core::Memory::InvalidateMemory` on decoded frames (`avplayer_source.cpp:682`) → rasterizer (host). | Keep in `bb-probe` (x86 FFmpeg). The invalidate becomes an async INVALIDATE{addr,size} message (same as the runtime's memory hooks), ~30/s during movies. |
| P9 | `sceGnmSetEmbedded{Ps,Vs}Shader` (not imported) | Write the address of static code arrays in the library image into SPI_SHADER_PGM_LO; the host would fetch code from a `bb-probe` address. | Not needed for Bloodborne. If ever needed: the host publishes the addresses of its own copies (static field), or the shaders are copied into shared guest memory at init. |
| P10 | `sceVideoOutWaitVblank` (not imported), vblank/flip events | Depend on host timing (PresentThread, frame limiter). | Shared vblank counter + `os_sync_wait_on_address`; events via P7. |
| P11 | `sceVideoOutGetDeviceCapabilityInfo`, `AdjustColor`, `ConfigureOutputMode_`, `RegisterBuffers` | Touch the presenter (Vulkan / swapchain / window). | Constants (HDR supported) published at host init; setters are async messages ordered on the submit ring. None touch SDL directly; the window only appears via `Presenter` construction in `GnmDriver::RegisterLib`, which moves to the host. |
| P12 | `sceGnmRequestFlipAndSubmitDone` (imported) | A stub returning `ORBIS_OK` today — if Bloodborne calls it, that flip/SubmitDone is silently dropped. Not split-specific. | Check with a call counter before M2; if it is called, implement it as SubmitDone + flip. |
| P13 | `sceGnmDrawIndexMultiInstanced`, `sceGnmDrawIndirectCountMulti` | `UNREACHABLE()` stubs (abort). Not imported. | None. |

Hot-path synchronous round trips: **none** with the handling above. Window/SDL: no HLE function
touches SDL directly. x86-specific code: only the TSC (P6).

## 7. Implied IPC

### 7.1 Submit ring (game → host, ordered, async, doorbell)

| Record | Payload | From |
|---|---|---|
| `SUBMIT_GFX` | count, dcb_addr[], dcb_bytes[], ccb_addr[], ccb_bytes[] (inline copy) | `sceGnmSubmitCommandBuffers(ForWorkload)` |
| `SUBMIT_GFX_FLIP` | as above + vo_handle, buf_idx, flip_mode, flip_arg | `sceGnmSubmitAndFlipCommandBuffers(ForWorkload)` (host does PatchFlipRequest, EOP-flip one-shot, init packet, `SubmitGfx`) |
| `SUBMIT_DONE` | — | `sceGnmSubmitDone` |
| `DING_DONG` | gnm_vqid, next_offs_dw | `sceGnmDingDong(ForWorkload)` |
| `VO_FLIP` | handle, buf_idx, flip_mode, flip_arg | `sceVideoOutSubmitFlip` |
| `VO_SET_FLIP_RATE`, `VO_CHANGE_ATTR`, `VO_UNREGISTER`, `VO_CONFIGURE_MODE`, `VO_ADJUST_COLOR` | small scalars / one BufferAttribute | VideoOut setters (ordered with flips) |
| `UNMAP_COMPUTE_QUEUE` | vqid | `sceGnmUnmapComputeQueue` |
| `INVALIDATE` | addr, size | AvPlayer frames, runtime memory hooks |

### 7.2 Control requests with a reply (load-time only)

`MAP_COMPUTE_QUEUE{pipe, queue, ring_base, ring_size_dw, read_ptr_addr} → vqid | error`,
`VO_OPEN → handle`, `VO_REGISTER_BUFFERS{start, n, addrs[n], BufferAttribute} → group | error`,
`VO_CLOSE{handle}`. Plus the non-HLE ones the runtime already calls through `bbgpu_*`
(`bbgpu_handle_fault`, overlay input capture, text input, flip/submit counters, hang dumps) —
outside this classification.

### 7.3 Event ring (host → game)

`IRQ{id}` for GfxEop (0x40), Compute0–6RelMem (0x00–0x06) and GpuIdle (0x09);
`FLIP{flip_arg, buffer index}`; `VBLANK{count}` at 60–120 Hz. The host forwards every
interrupt; `bb-probe` keeps all equeue registrations.

### 7.4 Minimal shared status block

| Field | Writer | Reader | Stale OK? | Notes |
|---|---|---|---|---|
| `submission_lock` (u32) | game sets 1, host clears 0 (release) + wake | SubmitDone / Submit* / DingDong wait; `sceGnmAreSubmitsAllowed` | waits: no; AreSubmitsAllowed: yes | `os_sync_wait_on_address` |
| `flip_status` (FlipStatus: count, process_time, tsc, submit_tsc, flip_arg, gc_queue_num, flip_pending_num, current_buffer) + seqlock | host (on flip); game (`flip_pending_num`/`gc_queue_num` atomics, `submit_tsc`) | `sceVideoOutGetFlipStatus`, `IsFlipPending`, queue-full check | yes for a racing flip; never torn | seqlock for the snapshot; the two counters are atomics touched by both sides |
| `vblank_status` (count, process_time, tsc) + seqlock | host | `GetVblankStatus`, `WaitVblank` (wait on `count`) | yes | not imported, cheap to have |
| Static, written once before "ready": label array address (or the labels themselves), `resolution` (SceVideoOutResolutionStatus), `hdr_supported`, vblank Hz | host | game | n/a | |
| Timebase: `process_start_ns`, guest TSC Hz, (tsc0, ns0) | game, at launch | host | n/a | P6 |
| Counters: flips, submissions | host | `probe.c` stats / watchdog | yes | replaces `bbgpu_get_flip_count` / `bbgpu_get_submit_count` |

Constants that need **no** field (compiled into both sides): GPU clock 800 MHz,
off-chip tessellation buffer size, tessellation factor ring address, Neo mode = 0.

## 8. Per-function tables

Columns: **BB** = imported by Bloodborne CUSA03173. *Frequency* is an estimate from code
structure and `docs/parallel_gpu.md` measurements, not a call trace. Sources are relative to the
repo root.

### GnmDriver — 67 registrations (excluding class N)

| Function | NID (library) | Source | BB | Class | State touched | Blocks | Frequency (guess) | Split handling / notes |
|---|---|---|:-:|:-:|---|---|---|---|
| `sceGnmAddEqEvent` | `b0xyllnVY-I` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:127` | **Y** | **E** | kernel `kqueues` (GetEqueue) + host `IrqC` handler for the IRQ id | no | load | Becomes game-local: add the event to the equeue in bb-probe; host posts every GfxEop / Compute*RelMem IRQ to the event ring, bb-probe dispatches to registered queues. No message needed. |
| `sceGnmAreSubmitsAllowed` | `b08AgtPlHPg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:163` | **Y** | **Q** | `submission_lock` (gnmdriver static) | no | per frame? | Shared word `submission_lock`; stale value acceptable (hint only). |
| `sceGnmBeginWorkload` | `ihxrbsoSKWc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:168` |  | **B** | none (pure, writes out-params) | no | load/rare | Pure helper; compile into bb-probe. |
| `sceGnmComputeWaitOnAddress` | `ffrNQOshows` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:176` |  | **B** | guest cmdbuf only | no | per dispatch | PM4 builder. |
| `sceGnmCreateWorkloadStream` | `5udAm+6boVg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:207` |  | **B** | none (pure, writes out-params) | no | load/rare | Pure helper; compile into bb-probe. |
| `sceGnmDeleteEqEvent` | `PVT+fuoS9gU` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:274` | **Y** | **E** | kernel `kqueues` + host `IrqC` unregister | no | load/shutdown | Game-local (see AddEqEvent). |
| `sceGnmDingDong` | `bX5IbRvECXk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:293` | **Y** | **S** | `liverpool->asc_queues`, `asc_next_offs_dw[]`, `m_submit_lock`, `submission_lock` (WaitGpuIdle), `Liverpool::SubmitAsc` | **yes** (WaitGpuIdle after SubmitDone) | per frame, several (async compute) | Ring record {vqid, next_offs_dw}; ring-wrap split and `asc_next_offs_dw` stay host-side. Wait on shared `submission_lock`. |
| `sceGnmDingDongForWorkload` | `byXlqupd8cE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:366` |  | **S** | `liverpool->asc_queues`, `asc_next_offs_dw[]`, `m_submit_lock`, `submission_lock` (WaitGpuIdle), `Liverpool::SubmitAsc` | **yes** (WaitGpuIdle after SubmitDone) | per frame, several (async compute) | Ring record {vqid, next_offs_dw}; ring-wrap split and `asc_next_offs_dw` stay host-side. Wait on shared `submission_lock`. |
| `sceGnmDispatchDirect` | `0BzLGljcwBo` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:376` |  | **B** | guest cmdbuf only | no | per dispatch | PM4 builder. |
| `sceGnmDispatchIndirect` | `Z43vKp5k7r0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:393` |  | **B** | guest cmdbuf only | no | per dispatch | PM4 builder. |
| `sceGnmDispatchIndirectOnMec` | `wED4ZXCFJT0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:409` |  | **B** | guest cmdbuf only | no | per dispatch | PM4 builder. |
| `sceGnmDispatchInitDefaultHardwareState` | `nF6bFRUBRAU` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:421` | **Y** | **B** | guest cmdbuf; `sceKernelIsNeoMode()` (constant 0) | no | per compute cmd buffer | PM4 builder. |
| `sceGnmDrawIndex` | `HlTPoZ-oY7Y` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:455` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw (not imported) | PM4 builder. Bloodborne builds its draws inline in its static libGnm, not through these. |
| `sceGnmDrawIndexAuto` | `GGsn7jMTxw4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:476` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw (not imported) | PM4 builder. Bloodborne builds its draws inline in its static libGnm, not through these. |
| `sceGnmDrawIndexIndirect` | `ED9-Fjr8Ta4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:490` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw (not imported) | PM4 builder. Bloodborne builds its draws inline in its static libGnm, not through these. |
| `sceGnmDrawIndexIndirectCountMulti` | `thbPcG7E7qk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:516` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw (not imported) | PM4 builder. Bloodborne builds its draws inline in its static libGnm, not through these. |
| `sceGnmDrawIndexIndirectMulti` | `5q95ravnueg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:553` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw (not imported) | PM4 builder. Bloodborne builds its draws inline in its static libGnm, not through these. |
| `sceGnmDrawIndexOffset` | `oYM+YzfCm2Y` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:588` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw (not imported) | PM4 builder. Bloodborne builds its draws inline in its static libGnm, not through these. |
| `sceGnmDrawIndirect` | `4v+otIIdjqg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:605` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw (not imported) | PM4 builder. Bloodborne builds its draws inline in its static libGnm, not through these. |
| `sceGnmDrawIndirectMulti` | `f5QQLp9rzGk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:636` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw (not imported) | PM4 builder. Bloodborne builds its draws inline in its static libGnm, not through these. |
| `sceGnmDrawInitDefaultHardwareState` | `Idffwf3yh8s` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:662` |  | **B** | guest cmdbuf; constexpr init tables (gnmdriver_init.h); `sceKernelIsNeoMode()` | no | per cmd buffer / frame | PM4 builder. |
| `sceGnmDrawInitDefaultHardwareState175` | `QhnyReteJ1M` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:687` |  | **B** | guest cmdbuf; constexpr init tables (gnmdriver_init.h); `sceKernelIsNeoMode()` | no | per cmd buffer / frame | PM4 builder. |
| `sceGnmDrawInitDefaultHardwareState200` | `0H2vBYbTLHI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:705` | **Y** | **B** | guest cmdbuf; constexpr init tables (gnmdriver_init.h); `sceKernelIsNeoMode()` | no | per cmd buffer / frame | PM4 builder. |
| `sceGnmDrawInitDefaultHardwareState350` | `yb2cRhagD1I` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:741` |  | **B** | guest cmdbuf; constexpr init tables (gnmdriver_init.h); `sceKernelIsNeoMode()` | no | per cmd buffer / frame | PM4 builder. |
| `sceGnmDrawInitToDefaultContextState` | `8lH54sfjfmU` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:777` |  | **B** | guest cmdbuf; constexpr tables | no | per cmd buffer | PM4 builder. |
| `sceGnmDrawInitToDefaultContextState400` | `im2ZuItabu4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:793` |  | **B** | guest cmdbuf; constexpr tables | no | per cmd buffer | PM4 builder. |
| `sceGnmEndWorkload` | `Fa3x75OOLRA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:885` |  | **B** | none (pure, writes out-params) | no | load/rare | Pure helper; compile into bb-probe. |
| `sceGnmGetEqEventType` | `UoYY0DWMC0U` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:928` | **Y** | **E** | none (reads `ev->data`) | no | per event | Pure helper; game-local. |
| `sceGnmGetGpuCoreClockFrequency` | `Fwvh++m9IQI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:944` |  | **Q** | none (`sceKernelIsNeoMode()` → 800 MHz) | no | load | Constant; game-local, no shared field needed. |
| `sceGnmGetOffChipTessellationBufferSize` | `FFVZcCu3zWU` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:967` |  | **Q** | constant 0x800000 | no | load | Constant; game-local. |
| `sceGnmGetTheTessellationFactorRingBufferBaseAddress` | `ln33zjBrfjk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1032` | **Y** | **Q** | `tessellation_factors_ring_addr` (lazy static) from shim `AddressSpace` constants | no | load | Constant 0x7F0000000 (shim/core/address_space.h); both processes must agree. Game-local. |
| `sceGnmInsertDingDongMarker` | `NfvOrNzy6sk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1054` |  | **B** | guest cmdbuf only | no | per pass (debug markers, 10s–100s/frame) | PM4 NOP builder. |
| `sceGnmInsertPopMarker` | `7qZVNgEu+SY` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1064` | **Y** | **B** | guest cmdbuf only | no | per pass (debug markers, 10s–100s/frame) | PM4 NOP builder. |
| `sceGnmInsertPushColorMarker` | `aPIZJTXC+cU` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1076` | **Y** | **B** | guest cmdbuf only | no | per pass (debug markers, 10s–100s/frame) | PM4 NOP builder. |
| `sceGnmInsertPushMarker` | `W1Etj-jlW7Y` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1099` | **Y** | **B** | guest cmdbuf only | no | per pass (debug markers, 10s–100s/frame) | PM4 NOP builder. |
| `sceGnmInsertSetMarker` | `jiItzS6+22g` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1125` | **Y** | **B** | guest cmdbuf only | no | per pass (debug markers, 10s–100s/frame) | PM4 NOP builder. |
| `sceGnmInsertWaitFlipDone` | `1qXLHIpROPE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1152` | **Y** | **B*** | guest cmdbuf; **VideoOut `port->buffer_labels` address** via `sceVideoOutGetBufferLabelAddress` | no | per frame | Builder that embeds the address of a host-heap label array. Needs labels at an address valid in both processes (see problems). |
| `sceGnmLogicalCuMaskToPhysicalCuMask` | `RU74kek-N0c` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1191` | **Y** | **B** | none (pure, writes out-params) | no | load/rare | Pure helper; compile into bb-probe. |
| `sceGnmMapComputeQueue` | `29oKvKXzEZo` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1201` | **Y** | **R** | `liverpool->asc_queues` (SlotVector insert); writes guest `*read_ptr_addr = 0` | no | load | Returns the vqid synchronously → sync round trip at load (fine), or allocate vqids on the game side and send async. |
| `sceGnmMapComputeQueueWithPriority` | `A+uGq+3KFtQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1238` | **Y** | **R** | `liverpool->asc_queues` (SlotVector insert); writes guest `*read_ptr_addr = 0` | no | load | Returns the vqid synchronously → sync round trip at load (fine), or allocate vqids on the game side and send async. |
| `sceGnmResetVgtControl` | `MYRtYhojKdA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1317` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw state change | PM4 builder. |
| `sceGnmSetCsShader` | `KXltnCwEJHQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1382` | **Y** | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSetCsShaderWithModifier` | `Kx-h-nWQJ8A` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1409` |  | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSetEmbeddedPsShader` | `X9Omw9dwv5M` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1442` |  | **B*** | guest cmdbuf; **static shader code arrays in the GPU library image** (address written into SPI_SHADER_PGM_LO) | no | rare (not imported) | Embeds a pointer to code in the library image: compiled into bb-probe it points at bb-probe memory the host cannot read. Put the embedded shaders in shared guest memory at init (or have the host recognise a fixed sentinel address). |
| `sceGnmSetEmbeddedVsShader` | `+AFvOEXrKJk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1511` |  | **B*** | guest cmdbuf; **static shader code arrays in the GPU library image** (address written into SPI_SHADER_PGM_LO) | no | rare (not imported) | Embeds a pointer to code in the library image: compiled into bb-probe it points at bb-probe memory the host cannot read. Put the embedded shaders in shared guest memory at init (or have the host recognise a fixed sentinel address). |
| `sceGnmSetEsShader` | `FUHG8sQ3R58` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1563` | **Y** | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSetGsShader` | `UJwNuMBcUAk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1599` | **Y** | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSetHsShader` | `VJNjFtqiF5w` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1627` | **Y** | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSetLsShader` | `vckdzbQ46SI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1657` | **Y** | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSetPsShader` | `bQVd5YzCal0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1690` | **Y** | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSetPsShader350` | `5uFKckiJYRM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1727` |  | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSetVgtControl` | `cFCp0NX8wf0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1794` |  | **B** | guest cmdbuf; `sceKernelIsNeoMode()` | no | per draw state change | PM4 builder. |
| `sceGnmSetVsShader` | `gAhCn6UiU4Y` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1825` | **Y** | **B** | guest cmdbuf only | no | **per shader bind (hot: 100s–1000s/frame)** | PM4 builder. The hottest HLE calls; must not IPC. |
| `sceGnmSubmitAndFlipCommandBuffers` | `xbxNatawohc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2249` | **Y** | **S** | `m_submit_lock`, `submission_lock`; PatchFlipRequest rewrites the last 64 dw of the guest DCB and calls `sceVideoOutSubmitEopFlip` (host `IrqC` one-shot GfxFlip handler) + label address; PerformSubmit: `send_init_packet`, `sdk_version`, `Liverpool::SubmitGfx` (draw-prep `Build(dcb)` runs on the caller thread today) | **yes** (WaitGpuIdle) | per frame (1) | Ring record {count, dcb/ccb addr+size arrays copied by value, vo_handle, buf_idx, flip_mode, flip_arg}. Validation + PatchFlipRequest can run game-side (pure on guest memory once labels are shared); flip-queue-full result from the shared flip counter. Init packet + EOP-flip registration host-side. |
| `sceGnmSubmitAndFlipCommandBuffersForWorkload` | `Ga6r7H6Y0RI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2258` |  | **S** | `m_submit_lock`, `submission_lock`; PatchFlipRequest rewrites the last 64 dw of the guest DCB and calls `sceVideoOutSubmitEopFlip` (host `IrqC` one-shot GfxFlip handler) + label address; PerformSubmit: `send_init_packet`, `sdk_version`, `Liverpool::SubmitGfx` (draw-prep `Build(dcb)` runs on the caller thread today) | **yes** (WaitGpuIdle) | per frame (1) | Ring record {count, dcb/ccb addr+size arrays copied by value, vo_handle, buf_idx, flip_mode, flip_arg}. Validation + PatchFlipRequest can run game-side (pure on guest memory once labels are shared); flip-queue-full result from the shared flip counter. Init packet + EOP-flip registration host-side. |
| `sceGnmSubmitCommandBuffers` | `zwY0YV91TTI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2348` | **Y** | **S** | `m_submit_lock`, `submission_lock`; PerformSubmit (`send_init_packet`, `sdk_version`, `Liverpool::SubmitGfx`) | **yes** (WaitGpuIdle) | **~65–80 per frame (~5000/s)** | Hot submit path: must be a lock-free ring push + doorbell, no reply. Validation is pure and stays game-side. The pointer arrays are guest memory but must be copied into the record (game may reuse them after return). |
| `sceGnmSubmitCommandBuffersForWorkload` | `jRcI8VcgTz4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2311` |  | **S** | `m_submit_lock`, `submission_lock`; PerformSubmit (`send_init_packet`, `sdk_version`, `Liverpool::SubmitGfx`) | **yes** (WaitGpuIdle) | **~65–80 per frame (~5000/s)** | Hot submit path: must be a lock-free ring push + doorbell, no reply. Validation is pure and stays game-side. The pointer arrays are guest memory but must be copied into the record (game may reuse them after return). |
| `sceGnmSubmitDone` | `yvZ73uQUqrk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2355` | **Y** | **S** | `m_submit_lock`, `submission_lock`, **`liverpool->IsGpuIdle()`** (sync read), `Liverpool::SubmitDone`, `send_init_packet`, `frames_submitted` | **yes** (WaitGpuIdle) | per frame (1) | Needs a GPU-idle decision synchronously; see problems. Proposed: game sets `submission_lock=1` and pushes a SubmitDone record; host clears it and posts GpuIdle when idle + retired. |
| `sceGnmUnmapComputeQueue` | `ArSg-TGinhk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2370` | **Y** | **R** | `liverpool->asc_queues` erase | no | shutdown | Async message (ordered after pending DingDongs). |
| `sceGnmUpdateGsShader` | `nLM2i2+65hA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2393` | **Y** | **B** | guest cmdbuf only | no | per shader bind (hot) | PM4 builder. |
| `sceGnmUpdateHsShader` | `GNlx+y7xPdE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2424` | **Y** | **B** | guest cmdbuf only | no | per shader bind (hot) | PM4 builder. |
| `sceGnmUpdatePsShader` | `4MgRw-bVNQU` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2457` | **Y** | **B** | guest cmdbuf only | no | per shader bind (hot) | PM4 builder. |
| `sceGnmUpdatePsShader350` | `mLVL7N7BVBg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2500` |  | **B** | guest cmdbuf only | no | per shader bind (hot) | PM4 builder. |
| `sceGnmUpdateVsShader` | `V31V01UiScY` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2545` | **Y** | **B** | guest cmdbuf only | no | per shader bind (hot) | PM4 builder. |
| `sceGnmDrawInitToDefaultContextStateInternalCommand` | `pF1HQjbmQJ0` (libSceGnmDriverCompat) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2861` |  | **B** | guest cmdbuf; **`sdk_version`** (gnmdriver static) | no | per cmd buffer | Needs `sdk_version` mirrored on the game side (bb-probe already knows it). |
| `sceGnmDrawInitToDefaultContextStateInternalSize` | `jajhf-Gi3AI` (libSceGnmDriverCompat) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2869` |  | **B** | **`sdk_version`** | no | load | Pure given `sdk_version`. |
| `sceGnmGetGpuCoreClockFrequency` | `Fwvh++m9IQI` (libSceGnmGetGpuCoreClockFrequency) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:944` |  | **Q** | none (`sceKernelIsNeoMode()` → 800 MHz) | no | load | Constant; game-local, no shared field needed. |

### VideoOut — 29 registrations (excluding class N)

| Function | NID (library) | Source | BB | Class | State touched | Blocks | Frequency (guess) | Split handling / notes |
|---|---|---|:-:|:-:|---|---|---|---|
| `sceVideoOutGetFlipStatus` | `SbU3dwp80lQ` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:244` | **Y** | **Q** | `port->flip_status` (count, process_time, tsc, submit_tsc, flip_arg, gc_queue_num, flip_pending_num, current_buffer) under `port_mutex` | no | per frame (Bloodborne imports it) | Shared FlipStatus behind a seqlock. Slightly stale is fine (a flip racing the read), torn is not. tsc/process_time written host-side need the guest timebase (see problems). |
| `sceVideoOutSubmitFlip` | `U46NwOiJpys` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:155` | **Y** | **S** | `port->flip_status` (pending count, submit_tsc via **rdtsc**) under `port_mutex`, `buffer_slots` validation, `liverpool->SendCommand` → `presenter->PrepareFrame` | no | per frame (if used) | Ring record {buf_idx, flip_mode, flip_arg}. Queue-full check (`flip_pending_num > 16`) against the shared atomic counter, game-side, so no reply is needed. |
| `sceVideoOutRegisterBuffers` | `w3BY+tAEiQY` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:125` | **Y** | **R** | port groups/buffer_slots, `buffer_labels` reset + `SignalVoLabel`, **`presenter->RegisterVideoOutSurface`** (Vulkan), FrameCapture | no (host does Vulkan work) | load / resolution change | Returns the group index → synchronous request/reply (load-time, fine). Game side keeps a mirror of slots for SubmitFlip validation. |
| `sceVideoOutAddFlipEvent` | `HXzjK9yI30k` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:41` | **Y** | **E** | driver port (handle check), kernel equeue AddEvent, **`port->flip_events`** (read by host `VideoOutDriver::Flip`) | no | load | Game-local: keep the flip-event equeue list in bb-probe; host posts a Flip record {flip_arg, buffer index} per flip, bb-probe triggers the queues. |
| `sceVideoOutAddVblankEvent` | `Xru92wHJRmg` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:83` |  | **E** | equeue AddEvent, **`port->vblank_events`** (read by host PresentThread) | no | load | Game-local; host posts a Vblank record {count} each tick (60–120 Hz). |
| `sceVideoOutSetFlipRate` | `CBiu4mCE1DA` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:141` | **Y** | **R** | `port->flip_rate` (read by host PresentThread) | no | load/rare | Async message or a shared field read by the host present thread. |
| `sceVideoOutSetBufferAttribute` | `i6-sR91Wt-4` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:22` | **Y** | **B** | guest struct only | no | load | Pure helper (fills a guest struct); game-local. |
| `sceVideoOutGetResolutionStatus` | `6kPnj51T62Y` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:287` |  | **Q** | `port->resolution` (set at driver construction from settings) | no | load | Constant after init → static field in shared block. |
| `sceVideoOutGetResolutionStatus` | `8XGijEoThE0` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:287` |  | **Q** | `port->resolution` (set at driver construction from settings) | no | load | Constant after init → static field in shared block. |
| `sceVideoOutAddVblankEvent` | `Ek+VR4lcJQI` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:83` |  | **E** | equeue AddEvent, **`port->vblank_events`** (read by host PresentThread) | no | load | Game-local; host posts a Vblank record {count} each tick (60–120 Hz). |
| `sceVideoOutOpen` | `Up36PTk687E` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:298` | **Y** | **R** | `driver->Open` (main_port.is_open), `liverpool->SetVoPort` | no | load (once) | Returns handle 1. Can answer locally and send an async Open message. |
| `sceVideoOutIsFlipPending` | `zgXifHT9ErY` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:147` |  | **Q** | `flip_status.flip_pending_num` | no | per frame? | Shared atomic; stale by µs is fine. |
| `sceVideoOutUnregisterBuffers` | `N5KDtkIjjJ4` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:324` |  | **R** | port groups/buffer_slots | no | rare | Async message (ordered on the flip ring). |
| `sceVideoOutGetBufferLabelAddress` | `OcQybQejHEY` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:333` |  | **Q** | **address of host-side `port->buffer_labels`** | no | load / per frame via Gnm | Returns a host heap pointer; must become an address valid in both processes (shared block at identical address). |
| `sceVideoOutClose` | `uquVH4-Du78` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:319` | **Y** | **R** | clears port, labels, groups; removes equeue events | no | shutdown | Message + game-side equeue cleanup. |
| `sceVideoOutGetVblankStatus` | `1FZBKy8HeNU` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:271` |  | **Q** | `port->vblank_status` under `vo_mutex` | no | per frame? (not imported) | Shared VblankStatus behind a seqlock; stale OK. |
| `sceVideoOutGetVblankStatus` | `d1AjT2uZJn0` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:271` |  | **Q** | `port->vblank_status` under `vo_mutex` | no | per frame? (not imported) | Shared VblankStatus behind a seqlock; stale OK. |
| `sceVideoOutGetDeviceCapabilityInfo` | `kGVLc3htQE8` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:362` |  | **Q** | **`presenter->IsHDRSupported()`**, ElfInfo PSF attrs | no | load | Constant after host init → field in shared block. |
| `sceVideoOutWaitVblank` | `j6RaAUlaLv0` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:374` |  | **E** | `port->vo_mutex`, `port->vblank_cv`, `vblank_status.count` | **yes** (until next vblank) | per frame if used (not imported) | Wait on the shared vblank counter (`os_sync_wait_on_address`), no message. |
| `sceVideoOutGetEventId` | `U2JJtSqNKZI` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:187` |  | **E** | none (reads the event struct) | no | per event | Pure helper; game-local. |
| `sceVideoOutGetEventData` | `rWUTcKdkUzQ` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:215` |  | **E** | none (reads the event struct) | no | per event | Pure helper; game-local. |
| `sceVideoOutGetEventCount` | `Mt4QHHkxkOc` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:232` |  | **E** | none (reads the event struct) | no | per event | Pure helper; game-local. |
| `sceVideoOutColorSettingsSetGamma` | `DYhhWbJSeRg` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:386` |  | **B** | guest struct only | no | load | Pure helper (fills a guest struct); game-local. |
| `sceVideoOutAdjustColor` | `pv9CI5VC+R0` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:394` |  | **R** | **`presenter->GetPPSettingsRef().gamma`** | no | rare (settings) | Async message. |
| `sceVideoOutDeleteFlipEvent` | `-Ozn0F1AFRg` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:68` |  | **E** | equeue RemoveEvent, `port->flip_events` | no | shutdown | Game-local. |
| `sceVideoOutDeleteVblankEvent` | `oNOQn3knW6s` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:110` |  | **E** | equeue RemoveEvent, `port->vblank_events` | no | shutdown | Game-local. |
| `sceVideoOutModeSetAny_` | `pjkDsgxli6c` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:419` |  | **B** | guest struct only | no | load | Pure helper (fills a guest struct); game-local. |
| `sceVideoOutConfigureOutputMode_` | `N1bEoJ4SRw4` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:424` |  | **R** | `port->is_hdr` (read by host Flip → `presenter->SetHDR`), ElfInfo | no | load | Async message; ordered before flips. |
| `sceVideoOutSubmitChangeBufferAttribute` | `IOdgHlCGU-k` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:452` |  | **R** | `port->groups[].attrib` under `port_mutex` (read by PrepareFrame) | no | rare | Must be ordered with flips → put on the flip/submit ring. |

### Kernel equeue — 17 registrations (excluding class N)

| Function | NID (library) | Source | BB | Class | State touched | Blocks | Frequency (guess) | Split handling / notes |
|---|---|---|:-:|:-:|---|---|---|---|
| `posix_kqueue` | `nh2IFMgKTv8` (libScePosix) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:327` |  | **E** | `kqueues` map (new EqueueInternal) | no | load | Runtime side (bb-probe). |
| `posix_kevent` | `RW-GEfpnsqg` (libScePosix) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:355` |  | **E** | `kqueues`, EqueueInternal events/timers (asio `io_context`) | **yes** (if nevents > 0) | per frame (if used) | Runtime side. |
| `sceKernelCreateEqueue` | `D0OdFMjp46I` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:425` | **Y** | **E** | `kqueues` map (new EqueueInternal) | no | load | Runtime side (bb-probe). |
| `sceKernelDeleteEqueue` | `jpFjmgAC5AE` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:456` | **Y** | **E** | `kqueues` map | no | shutdown | Runtime side. |
| `sceKernelWaitEqueue` | `fzyMKs9kim0` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:466` | **Y** | **E** | EqueueInternal mutex/cv, small timers (spin+yield), `runtime_thread_set_blocked` | **yes** | per frame, many threads | Runtime side; woken by bb-probe event-dispatch thread for host-posted events. |
| `sceKernelGetEventUserData` | `vz+pg2zdopI` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:612` |  | **E** | none (reads event struct) | no | per event | Pure helper. |
| `sceKernelAddUserEvent` | `4R6-OvI2cEA` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:580` |  | **E** | EqueueInternal events | no | varies | Runtime side, purely game-internal. |
| `sceKernelAddUserEventEdge` | `WDszmSbWuDk` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:596` |  | **E** | EqueueInternal events | no | varies | Runtime side, purely game-internal. |
| `sceKernelAddHRTimerEvent` | `R74tt43xP6k` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:498` |  | **E** | EqueueInternal + boost::asio timers on `bb:kernel_service` thread | no | varies | Runtime side; the kernel service thread moves with it. |
| `sceKernelDeleteHRTimerEvent` | `J+LF6LwObXU` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:534` |  | **E** | EqueueInternal + boost::asio timers on `bb:kernel_service` thread | no | varies | Runtime side; the kernel service thread moves with it. |
| `sceKernelAddTimerEvent` | `57ZK+ODEXWY` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:549` |  | **E** | EqueueInternal + boost::asio timers on `bb:kernel_service` thread | no | varies | Runtime side; the kernel service thread moves with it. |
| `sceKernelDeleteTimerEvent` | `YWQFUyXIVdU` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:570` |  | **E** | EqueueInternal + boost::asio timers on `bb:kernel_service` thread | no | varies | Runtime side; the kernel service thread moves with it. |
| `sceKernelTriggerUserEvent` | `F6e0kwo4cnk` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:621` |  | **E** | EqueueInternal events | no | varies | Runtime side, purely game-internal. |
| `sceKernelDeleteUserEvent` | `LJDwdSNTnDg` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:632` |  | **E** | EqueueInternal events | no | varies | Runtime side, purely game-internal. |
| `sceKernelGetEventId` | `mJ7aghmgvfc` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:617` |  | **E** | none (reads event struct) | no | per event | Pure helper. |
| `sceKernelGetEventFilter` | `23CPPI1tyBY` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:643` |  | **E** | none (reads event struct) | no | per event | Pure helper. |
| `sceKernelGetEventData` | `kwGyyjohI50` (libkernel) | `gpu/shadps4/core/libraries/kernel/equeue.cpp:647` |  | **E** | none (reads event struct) | no | per event | Pure helper. |

### AvPlayer — 23 registrations (excluding class N)

| Function | NID (library) | Source | BB | Class | State touched | Blocks | Frequency (guess) | Split handling / notes |
|---|---|---|:-:|:-:|---|---|---|---|
| `sceAvPlayerAddSource` | `KMcEa+rHsIo` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:14` | **Y** | **A** | AvPlayer object; `runtime_file_translate` or guest file_replacement callbacks; FFmpeg demux | no | per movie | bb-probe only (guest callbacks, runtime file system). |
| `sceAvPlayerAddSourceEx` | `x8uvuFOPZhU` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:22` |  | **A** | AvPlayer object; `runtime_file_translate` or guest file_replacement callbacks; FFmpeg demux | no | per movie | bb-probe only (guest callbacks, runtime file system). |
| `sceAvPlayerClose` | `NkJwDzKmIlw` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:37` | **Y** | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerCurrentTime` | `wwM99gjFf1Y` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:46` | **Y** | **A** | AvPlayer object | no | per frame during movies | bb-probe only. |
| `sceAvPlayerDisableStream` | `BOVKAzRmuTQ` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:54` |  | **A** | none (handle check only) | no | rare | No-op; bb-probe. |
| `sceAvPlayerEnableStream` | `ODJK2sn9w4A` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:62` | **Y** | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerGetAudioData` | `Wnp1OVcrZgk` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:70` | **Y** | **A** | AvPlayer object | no | per frame during movies | bb-probe only. |
| `sceAvPlayerGetStreamInfo` | `d8FcbzfAdQw` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:78` | **Y** | **A** | AvPlayer object | no | per frame during movies | bb-probe only. |
| `sceAvPlayerGetVideoData` | `o3+RWnHViSg` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:87` |  | **A** | frame queue; decoded NV12 written into guest texture memory (guest allocate_texture); **`Core::Memory::InvalidateMemory`** → rasterizer | no | per video frame (~30/s during movies) | bb-probe; the invalidate becomes an async "invalidate range" message to the host. |
| `sceAvPlayerGetVideoDataEx` | `JdksQu8pNdQ` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:95` | **Y** | **A** | frame queue; decoded NV12 written into guest texture memory (guest allocate_texture); **`Core::Memory::InvalidateMemory`** → rasterizer | no | per video frame (~30/s during movies) | bb-probe; the invalidate becomes an async "invalidate range" message to the host. |
| `sceAvPlayerInit` | `aS66RI0gGgo` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:104` | **Y** | **A** | new AvPlayer (guest allocator/file/event callbacks); starts controller thread (`runtime_thread_attach_host`) | no | per movie | bb-probe only. |
| `sceAvPlayerInitEx` | `o9eWRkSL+M4` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:121` |  | **A** | new AvPlayer (guest allocator/file/event callbacks); starts controller thread (`runtime_thread_attach_host`) | no | per movie | bb-probe only. |
| `sceAvPlayerIsActive` | `UbQoYawOsfY` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:147` | **Y** | **A** | AvPlayer object | no | per frame during movies | bb-probe only. |
| `sceAvPlayerJumpToTime` | `XC9wM+xULz8` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:155` |  | **A** | none (handle check only) | no | rare | No-op; bb-probe. |
| `sceAvPlayerPause` | `9y5v+fGN4Wk` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:163` | **Y** | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerPostInit` | `HD1YKVU26-M` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:171` | **Y** | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerResume` | `w5moABNwnRY` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:184` | **Y** | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerSetAvSyncMode` | `k-q+xOxdc3E` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:192` |  | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerSetLooping` | `OVths0xGfho` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:205` | **Y** | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerSetTrickSpeed` | `av8Z++94rs0` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:216` |  | **A** | none (handle check only) | no | rare | No-op; bb-probe. |
| `sceAvPlayerStart` | `ET4Gr-Uu07s` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:224` | **Y** | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerStop` | `ZC17w3vB5Lo` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:232` | **Y** | **A** | AvPlayer state machine / decoder threads | Stop/Close join threads | per movie | bb-probe only. |
| `sceAvPlayerStreamCount` | `hdTyRzCXQeQ` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:240` | **Y** | **A** | AvPlayer object | no | per frame during movies | bb-probe only. |

### Class N (stubs / not available in retail firmware)

All return a constant and touch no state; they run in `bb-probe` unchanged.

#### GnmDriver — 187 class-N registrations

| Function | NID (library) | Source | BB | Returns |
|---|---|---|:-:|---|
| `sceGnmComputeWaitSemaphore` | `EJapNl2+pgU` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:202` |  | `ORBIS_OK` |
| `sceGnmDebuggerGetAddressWatch` | `jwCEzr7uEP4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:215` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebuggerHaltWavefront` | `PNf0G7gvFHQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:221` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebuggerReadGds` | `nO-tMnaxJiE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:227` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebuggerReadSqIndirectRegister` | `t0HIQWnvK9E` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:233` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebuggerResumeWavefront` | `HsLtF4jKe48` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:239` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebuggerResumeWavefrontCreation` | `JRKSSV0YzwA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:245` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebuggerSetAddressWatch` | `jpTMyYB8UBI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:251` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebuggerWriteGds` | `MJG69Q7ti+s` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:257` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebuggerWriteSqIndirectRegister` | `PaFw9w6f808` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:263` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebugHardwareStatus` | `qpGITzPE+Zc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:269` | **Y** | `ORBIS_OK` |
| `sceGnmDestroyWorkloadStream` | `UtObDRQiGbs` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:288` |  | `ORBIS_OK` |
| `sceGnmDisableMipStatsReport` | `HHo1BAljZO8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:371` | **Y** | `ORBIS_OK` |
| `sceGnmDrawIndexMultiInstanced` | `jHdPvIzlpKc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:582` |  | `aborts (UNREACHABLE)` |
| `sceGnmDrawIndirectCountMulti` | `cUCo8OvArrw` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:630` |  | `aborts (UNREACHABLE)` |
| `sceGnmDrawOpaqueAuto` | `stDSYW2SBVM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:814` |  | `ORBIS_OK` |
| `sceGnmDriverCaptureInProgress` | `TLV4mswiZ4A` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:819` | **Y** | `false` |
| `sceGnmDriverInternalRetrieveGnmInterface` | `ODEeJ1GfDtE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:825` |  | `0x80000000` |
| `sceGnmDriverInternalRetrieveGnmInterfaceForGpuDebugger` | `4LSXsEKPTsE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:831` |  | `0x80000000` |
| `sceGnmDriverInternalRetrieveGnmInterfaceForGpuException` | `MpncRjHNYRE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:837` |  | `0x80000000` |
| `sceGnmDriverInternalRetrieveGnmInterfaceForHDRScopes` | `EwjWGcIOgeM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:843` |  | `0x80000000` |
| `sceGnmDriverInternalRetrieveGnmInterfaceForReplay` | `3EXdrVC7WFk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:849` |  | `0x80000000` |
| `sceGnmDriverInternalRetrieveGnmInterfaceForResourceRegistration` | `P9iKqxAGeck` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:855` |  | `0x80000000` |
| `sceGnmDriverInternalRetrieveGnmInterfaceForValidation` | `t-vIc5cTEzg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:861` |  | `0x80000000` |
| `sceGnmDriverInternalVirtualQuery` | `BvvO8Up88Zc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:867` |  | `ORBIS_OK` |
| `sceGnmDriverTraceInProgress` | `R6z1xM3pW-w` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:873` |  | `false` |
| `sceGnmDriverTriggerCapture` | `d88anrgNoKY` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:879` | **Y** | `ORBIS_GNM_ERROR_CAPTURE_RAZOR_NOT_LOADED` |
| `sceGnmFindResourcesPublic` | `4Mv9OXypBG8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:892` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmFlushGarlic` | `iBt3Oe00Kvc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:898` | **Y** | `void` |
| `sceGnmGetCoredumpAddress` | `GviyYfFQIkc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:902` |  | `ORBIS_OK` |
| `sceGnmGetCoredumpMode` | `meiO-5ZCVIE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:907` |  | `ORBIS_OK` |
| `sceGnmGetCoredumpProtectionFaultTimestamp` | `O-7nHKgcNSQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:912` |  | `ORBIS_OK` |
| `sceGnmGetDbgGcHandle` | `bSJFzejYrJI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:917` |  | `-1` |
| `sceGnmGetDebugTimestamp` | `pd4C7da6sEg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:923` |  | `ORBIS_OK` |
| `sceGnmGetEqTimeStamp` | `H7-fgvEutM0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:933` | **Y** | `ORBIS_OK` |
| `sceGnmGetGpuBlockStatus` | `oL4hGI1PMpw` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:938` |  | `ORBIS_OK` |
| `sceGnmGetGpuInfoStatus` | `tZCSL5ulnB4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:950` |  | `ORBIS_OK` |
| `sceGnmGetLastWaitedAddress` | `iFirFzgYsvw` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:956` |  | `ORBIS_OK` |
| `sceGnmGetNumTcaUnits` | `KnldROUkWJY` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:962` |  | `ORBIS_OK` |
| `sceGnmGetOwnerName` | `QJjPjlmPAL0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:972` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmGetPhysicalCounterFromVirtualized` | `dewXw5roLs0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:978` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmGetProtectionFaultTimeStamp` | `fzJdEihTFV4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:984` |  | `0` |
| `sceGnmGetResourceBaseAddressAndSizeInBytes` | `4PKnYXOhcx4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:990` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmGetResourceName` | `O0S96YnD04U` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:996` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmGetResourceShaderGuid` | `UBv7FkVfzcQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1002` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmGetResourceType` | `bdqdvIkLPIU` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1008` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmGetResourceUserData` | `UoBuWAhKk7U` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1014` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmGetShaderProgramBaseAddress` | `nEyFbYUloIM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1020` |  | `ORBIS_OK` |
| `sceGnmGetShaderStatus` | `k7iGTvDQPLQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1026` |  | `ORBIS_OK` |
| `sceGnmGpuPaDebugEnter` | `QLdG7G-PBZo` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1044` |  | `void` |
| `sceGnmGpuPaDebugLeave` | `tVEdZe3wlbY` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1049` |  | `void` |
| `sceGnmInsertSetColorMarker` | `aj3L-iaFmyk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1120` | **Y** | `ORBIS_OK` |
| `sceGnmInsertThreadTraceMarker` | `URDgJcXhQOs` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1146` |  | `ORBIS_OK` |
| `sceGnmIsCoredumpValid` | `HRyNHoAjb6E` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1174` |  | `ORBIS_OK` |
| `sceGnmIsUserPaEnabled` | `jg33rEKLfVs` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1179` | **Y** | `false` |
| `sceGnmLogicalCuIndexToPhysicalCuIndex` | `26PM5Mzl8zc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1185` | **Y** | `ORBIS_OK` |
| `sceGnmLogicalTcaUnitToPhysical` | `Kl0Z3LH07QI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1196` |  | `ORBIS_OK` |
| `sceGnmPaDisableFlipCallbacks` | `+N+wrSYBLIw` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1247` |  | `ORBIS_OK` |
| `sceGnmPaEnableFlipCallbacks` | `8WDA9RiXLaw` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1253` |  | `ORBIS_OK` |
| `sceGnmPaHeartbeat` | `tNuT48mApTc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1259` |  | `ORBIS_OK` |
| `sceGnmQueryResourceRegistrationUserMemoryRequirements` | `6IMbpR7nTzA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1265` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmRaiseUserExceptionEvent` | `+rJnw2e9O+0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1271` |  | `ORBIS_OK` |
| `sceGnmRegisterGdsResource` | `9Mv61HaMhfA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1277` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmRegisterGnmLiveCallbackConfig` | `t7-VbMosbR4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1283` |  | `void` |
| `sceGnmRegisterOwner` | `ZFqKFl23aMc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1288` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmRegisterResource` | `nvEwfYAImTs` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1294` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmRequestFlipAndSubmitDone` | `gObODli-OH8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1302` | **Y** | `ORBIS_OK` |
| `sceGnmRequestFlipAndSubmitDoneForWorkload` | `6YRHhh5mHCs` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1307` |  | `ORBIS_OK` |
| `sceGnmRequestMipStatsReportAndReset` | `f85orjx7qts` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1312` | **Y** | `ORBIS_OK` |
| `sceGnmSdmaClose` | `hS0MKPRdNr0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1334` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSdmaConstFill` | `31G6PB2oRYQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1340` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSdmaCopyLinear` | `Lg2isla2XeQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1346` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSdmaCopyTiled` | `-Se2FY+UTsI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1352` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSdmaCopyWindow` | `OlFgKnBsALE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1358` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSdmaFlush` | `LQQN0SwQv8c` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1364` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSdmaGetMinCmdSize` | `suUlSjWr7CE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1370` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSdmaOpen` | `5AtqyMgO7fM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1376` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSetGsRingSizes` | `jtkqXpAOY6w` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1594` | **Y** | `ORBIS_OK` |
| `sceGnmSetResourceRegistrationUserMemory` | `q-qhDxP67Hg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1765` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSetResourceUserData` | `K3BKBBYKUSE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1771` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSetSpiEnableSqCounters` | `0O3xxFaiObw` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1777` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSetSpiEnableSqCountersForUnitInstance` | `lN7Gk-p9u78` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1783` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSetupMipStatsReport` | `+xuDhxlWRPg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1789` | **Y** | `ORBIS_OK` |
| `sceGnmSetWaveLimitMultiplier` | `y+iI2lkX+qI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1858` |  | `ORBIS_OK` |
| `sceGnmSetWaveLimitMultipliers` | `XiyzNZ9J4nQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1863` |  | `ORBIS_OK` |
| `sceGnmSpmEndSpm` | `kkn+iy-mhyg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1868` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmInit` | `aqhuK2Mj4X4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1874` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmInit2` | `KHpZ9hJo1c0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1880` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmSetDelay` | `QEsMC+M3yjE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1886` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmSetMuxRam` | `hljMAxTLNF0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1892` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmSetMuxRam2` | `bioGsp74SLM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1898` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmSetSelectCounter` | `cMWWYeqQQlM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1904` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmSetSpmSelects` | `-zJi8Vb4Du4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1910` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmSetSpmSelects2` | `xTsOqp-1bE4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1916` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSpmStartSpm` | `AmmYLcJGTl0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1922` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttFini` | `UHDiSFDxNao` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1928` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttFinishTrace` | `a3tLC56vwug` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1934` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetBcInfo` | `L-owl1dSKKg` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1940` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetGpuClocks` | `LQtzqghKQm4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1946` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetHiWater` | `wYN5mmv6Ya8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1952` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetStatus` | `9X4SkENMS0M` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1958` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetTraceCounter` | `lbMccQM2iqc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1964` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetTraceWptr` | `DYAC6JUeZvM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1970` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetWrapCounts` | `pS2tjBxzJr4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1976` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetWrapCounts2` | `rXV8az6X+fM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1982` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttGetWritebackLabels` | `ARS+TNLopyk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1988` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttInit` | `X6yCBYPP7HA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:1994` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSelectMode` | `2IJhUyK8moE` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2000` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSelectTarget` | `QA5h6Gh3r60` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2006` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSelectTokens` | `F5XJY1XHa3Y` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2012` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSetCuPerfMask` | `wJtaTpNZfH4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2018` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSetDceEventWrite` | `kY4dsQh+SH4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2024` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSetHiWater` | `7XRH1CIfNpI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2030` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSetTraceBuffer2` | `05YzC2r3hHo` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2036` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSetTraceBuffers` | `ASUric-2EnI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2042` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSetUserData` | `gPxYzPp2wlo` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2048` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSetUserdataTimer` | `d-YcZX7SIQA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2054` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttStartTrace` | `ru8cb4he6O8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2060` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttStopTrace` | `gVuGo1nBnG8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2066` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSwitchTraceBuffer` | `OpyolX6RwS0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2072` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttSwitchTraceBuffer2` | `dl5u5eGBgNk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2078` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmSqttWaitForEvent` | `QLzOwOF0t+A` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2084` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmUnregisterAllResourcesForOwner` | `yhFCnaz5daw` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2375` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmUnregisterOwnerAndResources` | `fhKwCVVj9nk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2381` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmUnregisterResource` | `k8EXkhIP+lM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2387` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmValidateCommandBuffers` | `iCO804ZgzdA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2583` | **Y** | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceGnmValidateDisableDiagnostics` | `SXw4dZEkgpA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2589` |  | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceGnmValidateDisableDiagnostics2` | `BgM3t3LvcNk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2595` |  | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceGnmValidateDispatchCommandBuffers` | `qGP74T5OWJc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2601` |  | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceGnmValidateDrawCommandBuffers` | `hsZPf1lON7E` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2607` |  | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceGnmValidateGetDiagnosticInfo` | `RX7XCNSaL6I` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2613` |  | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceGnmValidateGetDiagnostics` | `5SHGNwLXBV4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2619` |  | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceGnmValidateGetVersion` | `HzMN7ANqYEc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2625` |  | `0` |
| `sceGnmValidateOnSubmitEnabled` | `rTIV11nMQuM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2631` |  | `false` |
| `sceGnmValidateResetState` | `MBMa6EFu4Ko` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2637` |  | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceGnmValidationRegisterMemoryCheckCallback` | `Q7t4VEYLafI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2643` |  | `ORBIS_GNM_ERROR_VALIDATION_NOT_ENABLED` |
| `sceRazorCaptureCommandBuffersOnlyImmediate` | `xeTLfxVIQO4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2649` | **Y** | `ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL` |
| `sceRazorCaptureCommandBuffersOnlySinceLastFlip` | `9thMn+uB1is` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2655` | **Y** | `ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL` |
| `sceRazorCaptureImmediate` | `u9YKpRRHe-M` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2661` | **Y** | `ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL` |
| `sceRazorCaptureSinceLastFlip` | `4UFagYlfuAM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2667` | **Y** | `ORBIS_GNM_ERROR_CAPTURE_FAILED_INTERNAL` |
| `sceRazorIsLoaded` | `f33OrruQYbM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2673` | **Y** | `false` |
| `Func_063D065A2D6359C3` | `Bj0GWi1jWcM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2679` |  | `ORBIS_OK` |
| `Func_0CABACAFB258429D` | `DKusr7JYQp0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2684` |  | `ORBIS_OK` |
| `Func_150CF336FC2E99A3` | `FQzzNvwumaM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2689` |  | `ORBIS_OK` |
| `Func_17CA687F9EE52D49` | `F8pof57lLUk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2694` |  | `ORBIS_OK` |
| `Func_1870B89F759C6B45` | `GHC4n3Wca0U` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2699` |  | `ORBIS_OK` |
| `Func_26F9029EF68A955E` | `JvkCnvaKlV4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2704` |  | `ORBIS_OK` |
| `Func_301E3DBBAB092DB0` | `MB49u6sJLbA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2709` |  | `ORBIS_OK` |
| `Func_30BAFE172AF17FEF` | `MLr+Fyrxf+8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2714` |  | `ORBIS_OK` |
| `Func_3E6A3E8203D95317` | `Pmo+ggPZUxc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2719` |  | `ORBIS_OK` |
| `Func_40FEEF0C6534C434` | `QP7vDGU0xDQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2724` |  | `ORBIS_OK` |
| `Func_416B9079DE4CBACE` | `QWuQed5Mus4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2729` |  | `ORBIS_OK` |
| `Func_4774D83BB4DDBF9A` | `R3TYO7Tdv5o` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2734` |  | `ORBIS_OK` |
| `Func_50678F1CCEEB9A00` | `UGePHM7rmgA` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2739` |  | `ORBIS_OK` |
| `Func_54A2EC5FA4C62413` | `VKLsX6TGJBM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2744` |  | `ORBIS_OK` |
| `Func_5A9C52C83138AE6B` | `WpxSyDE4rms` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2749` |  | `ORBIS_OK` |
| `Func_5D22193A31EA1142` | `XSIZOjHqEUI` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2754` |  | `ORBIS_OK` |
| `Func_725A36DEBB60948D` | `clo23rtglI0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2759` |  | `ORBIS_OK` |
| `Func_8021A502FA61B9BB` | `gCGlAvphubs` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2764` |  | `ORBIS_OK` |
| `Func_9D002FE0FA40F0E6` | `nQAv4PpA8OY` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2769` |  | `ORBIS_OK` |
| `Func_9D297F36A7028B71` | `nSl-NqcCi3E` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2774` |  | `ORBIS_OK` |
| `Func_A2D7EC7A7BCF79B3` | `otfsenvPebM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2779` |  | `ORBIS_OK` |
| `Func_AA12A3CB8990854A` | `qhKjy4mQhUo` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2784` |  | `ORBIS_OK` |
| `Func_ADC8DDC005020BC6` | `rcjdwAUCC8Y` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2789` |  | `ORBIS_OK` |
| `Func_B0A8688B679CB42D` | `sKhoi2ectC0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2794` |  | `ORBIS_OK` |
| `Func_B489020B5157A5FF` | `tIkCC1FXpf8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2799` |  | `ORBIS_OK` |
| `Func_BADE7B4C199140DD` | `ut57TBmRQN0` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2804` |  | `ORBIS_OK` |
| `Func_D1511B9DCFFB3DD9` | `0VEbnc-7Pdk` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2809` |  | `ORBIS_OK` |
| `Func_D53446649B02E58E` | `1TRGZJsC5Y4` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2814` |  | `ORBIS_OK` |
| `Func_D8B6E8E28E1EF0A3` | `2Lbo4o4e8KM` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2819` |  | `ORBIS_OK` |
| `Func_D93D733A19DD7454` | `2T1zOhnddFQ` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2824` |  | `ORBIS_OK` |
| `Func_DE995443BC2A8317` | `3plUQ7wqgxc` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2829` |  | `ORBIS_OK` |
| `Func_DF6E9528150C23FF` | `326VKBUMI-8` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2834` |  | `ORBIS_OK` |
| `Func_ECB4C6BA41FE3350` | `7LTGukH+M1A` (libSceGnmDriver) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2839` |  | `ORBIS_OK` |
| `sceGnmDebugModuleReset` | `dqPBvjFVpTA` (libSceGnmDebugModuleReset) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2844` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmDebugReset` | `RNPAItiMLIg` (libSceGnmDebugReset) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2850` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `Func_C4C328B7CF3B4171` | `xMMot887QXE` (libSceGnmDebugReset) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2856` |  | `ORBIS_OK` |
| `sceGnmFindResources` | `vbcR4Ken6AA` (libSceGnmDriverResourceRegistration) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2877` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmGetResourceRegistrationBuffers` | `eLQbNsKeTkU` (libSceGnmDriverResourceRegistration) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2883` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `sceGnmRegisterOwnerForSystem` | `j6mSQs3UgaY` (libSceGnmDriverResourceRegistration) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2889` |  | `ORBIS_GNM_ERROR_FAILURE` |
| `Func_1C43886B16EE5530` | `HEOIaxbuVTA` (libSceGnmDriverResourceRegistration) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2895` |  | `ORBIS_OK` |
| `Func_81037019ECCD0E01` | `gQNwGezNDgE` (libSceGnmDriverResourceRegistration) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2900` |  | `ORBIS_OK` |
| `Func_BFB41C057478F0BF` | `v7QcBXR48L8` (libSceGnmDriverResourceRegistration) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2905` |  | `ORBIS_OK` |
| `Func_E51D44DB8151238C` | `5R1E24FRI4w` (libSceGnmDriverResourceRegistration) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2910` |  | `ORBIS_OK` |
| `Func_F916890425496553` | `+RaJBCVJZVM` (libSceGnmDriverResourceRegistration) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2915` |  | `ORBIS_OK` |
| `Func_4774D83BB4DDBF9A` | `R3TYO7Tdv5o` (libSceGnmWaitFreeSubmit) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2734` |  | `ORBIS_OK` |
| `Func_BADE7B4C199140DD` | `ut57TBmRQN0` (libSceGnmWaitFreeSubmit) | `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp:2804` |  | `ORBIS_OK` |

#### VideoOut — 1 class-N registrations

| Function | NID (library) | Source | BB | Returns |
|---|---|---|:-:|---|
| `sceVideoOutSetWindowModeMargins` | `MTxxrOCeSig` (libSceVideoOut) | `gpu/shadps4/core/libraries/videoout/video_out.cpp:462` |  | `ORBIS_OK` |

#### AvPlayer — 3 class-N registrations

| Function | NID (library) | Source | BB | Returns |
|---|---|---|:-:|---|
| `sceAvPlayerChangeStream` | `buMCiJftcfw` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:32` |  | `ORBIS_OK` |
| `sceAvPlayerSetLogCallback` | `eBTreZ84JFY` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:200` |  | `ORBIS_OK` |
| `sceAvPlayerVprintf` | `yN7Jhuv8g24` (libSceAvPlayer) | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp:248` |  | `ORBIS_OK` |

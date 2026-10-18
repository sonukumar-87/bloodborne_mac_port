# Metal-native renderer (0.5.0) — design

Date: 2026-10-16. Branch: `metal-native` (from `metal-backend`). Status: spec, awaiting review.

## Goal

0.5.0 renders through Metal only. MoltenVK and the Vulkan loader are gone from the build and the
release app; there is no Vulkan instance, device, queue or command buffer anywhere.

The user plays 0.4.0 meanwhile, so this branch may be broken between milestones. There is no
backend interface and no Vulkan fallback: the renderer is converted in place (approach "R1",
agreed 2026-10-16), keeping the logic that makes ours fast (two-stage draw pipeline, upload
hoisting, live resolution, background pipeline compiles, the fixes of the past week).

### Success criteria (0.5.0 ships when all hold)

1. The release app contains no `libMoltenVK`, no `libvulkan`, no Vulkan ICD file; nothing calls
   a `vk*` function (checked with `nm -u` on every binary).
2. Picture matches 0.4.0 on the test scenes (below), including the geometry-shader effects
   0.4.0 skips (they must now be drawn).
3. Speed at least 0.4.0's on the head-to-head protocol (Hunter's Dream headstone, uncapped,
   warm caches): 1080p native ≥ 94 FPS, 1440p native ≥ 66, 1080p FSR 3.1 Quality ≥ 107,
   1080p MetalFX Quality ≥ 106. At the 60 cap: late frames per 5 s no worse than 0.4.0.
4. Upscalers offered: Off, MetalFX, FSR 3.1, TAA (the launcher's list today).
5. A 15-minute walk on each test save without a crash or a Metal validation error
   (`MTL_DEBUG_LAYER=1` on the title screen and a save load).

### Not in scope

- Performance work beyond parity: pass merging, faster tessellation, zero-copy guest memory.
  These come after 0.5.0 (the design keeps room for them, see "Later").
- FSR 4 and FSR 4.1.1: not supported on Macs today (`fsr4_supported` is false); their code is
  removed from the macOS build, the settings keys stay readable.
- Single-process mode (`BB_GPU_PROCESS=0`): removed. Split mode is the default since
  2026-10-12 and is what the renderer is built for (arm64 host).
- Upstream mergeability: we have diverged; files may be restructured freely.

## Architecture

Unchanged around the renderer: the game runs as x86-64 under Rosetta (`bb-probe`), the GPU
work runs in the native arm64 host (`bbgpu-host`) which maps guest memory at the same addresses,
PM4 parsing and the draw pipeline (stage A parse, stage B `bb:DrawRec`) stay as they are.

What changes is everything below stage B:

```
stage B (DrawRec)  ──Record(lambda(Encoder&))──▶  recorder thread (bb:MtlRecorder)
   rasterizer, caches, upscalers                     Metal::Encoder: render/compute/blit
                                                     encoders on one MTLCommandBuffer
                                                     ──commit + signal tick──▶ MTLCommandQueue
presenter: CAMetalLayer drawable ◀── final blit / upscaler / overlay
```

Vulkan stays only as **vocabulary**: header-only enums and plain structs (`vk::Format`,
`vk::BufferCopy`, `vk::BufferImageCopy`, `vk::ComponentMapping`, blend/compare enums) from
Vulkan-Headers, because the shader recompiler, `liverpool_to_vk` and the pipeline keys speak
them. No Vulkan handle type survives (`vk::Buffer`, `vk::Image`, `vk::CommandBuffer`, …).

### Components

**1. Device, queue, ticks** (`renderer_metal/mtl_device.*`, replaces `vk_instance`,
`vk_platform`, `vk_master_semaphore`/`vk_semaphore`, `vk_resource_pool`)
- `MTLCreateSystemDefaultDevice()` in the host; one `MTLCommandQueue`.
- Ticks keep today's contract (monotonic u64 per submission; `CurrentTick`, `IsFree(tick)`,
  `Wait(tick)`, `Finish`, deferred operations run once a tick is done): an `MTLSharedEvent`
  signalled with the tick at the end of each command buffer; `IsFree` reads `signaledValue`,
  `Wait` uses `waitUntilSignaledValue`, a completion handler runs deferred operations.
- Limits and format support from a table filled once (alignments: 256 for uniforms and
  storage, sampler anisotropy 16, LOD bias, max buffer length), replacing `Instance` queries.
- Memory budget queries (texture cache eviction): `device.currentAllocatedSize` against
  `recommendedMaxWorkingSetSize`.

**2. Command recording** (`renderer_metal/mtl_encoder.*` from `mtl_frame.mm`; `vk_scheduler` →
`mtl_scheduler`)
- `Scheduler::Record` keeps its shape and threading (chunks, `RecordData`, `KickRecording`,
  host copies on the recorder thread, `WaitHostCopies`), but closures take a `Metal::Encoder&`
  instead of a `vk::CommandBuffer`.
- `Metal::Encoder` is the state machine `mtl_frame.mm` already implements, now called directly:
  render passes opened at the first draw (passes without draws or clears cost nothing),
  consecutive dispatches in one compute encoder, blits, tessellation runs batched, geometry
  emulation, per-encoder state and binding cache, unretained-reference command buffers, a
  residency set for long-lived buffers.
- Barriers: resources are hazard-tracked (Metal orders encoders), so `vk_barrier_tracker`,
  image layouts and `GetBarriers` are deleted. Untracked resources and fences are a "Later"
  optimisation.
- Submission: commit with the tick signal; `RecordHoistedCopy` (upload hoisting) becomes a blit
  encoder placed before the pass that needs it, as today.

**3. Resources and caches** (`buffer_cache/`, `texture_cache/`, `vk_runtime` → `mtl_runtime`,
`vk_staging_buffer_pool` → staging in `mtl_runtime`)
- `Buffer`/`UniqueBuffer` own an `id<MTLBuffer>` (via an opaque C++ wrapper, so shared headers
  stay plain C++) and its `gpuAddress`; VMA is gone. Memory types map to storage modes:
  DeviceLocal → private, Upload/HostCached/Stream → shared.
- **Guest arena:** one private `MTLBuffer` of 16 GiB (`BB_ARENA_GIB`, clamped to
  `maxBufferLength`) at the same place in the design as today; the BDA page table holds
  `gpuAddress + offset`. Guest data still arrives through staging copies on the copy threads
  (same write-tracking, fault manager and hoisting), so CPU/GPU ordering is unchanged.
- `Image` owns an `id<MTLTexture>` (private storage); `ImageView` a texture view
  (`newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:`); `Sampler` an
  `MTLSamplerState`. Format and swizzle conversion from `vk_to_mtl` (exists). 1D images are
  2D textures (as MoltenVK does). Formats Metal lacks keep today's fallbacks
  (`host_compatibility` re-keyed to Metal pixel formats).
- Transfers (`CopyBuffer`, `FillBuffer`, `UploadImage`, `DownloadImage`, `CopyImage`,
  `CopyMip`, `CopyColorAndDepth`, `SetBackingSamples`) become blit-encoder or kernel calls in
  `mtl_runtime`.
- The caches' own GPU work: detiling (`tile_manager`, 12 kernels + variants), the fault-buffer
  parser and the MSAA/depth blits (`blit_helper`) become Metal compute/render pipelines built
  from the same GLSL host shaders (glslang → SPIR-V at build time, as now → MSL at start through
  our SPIRV-Cross compiler, `mtl_shader`). Specialisation constants become function constants.
- Linux-only sparse-arena code is deleted.

**4. Pipelines and shaders** (`vk_pipeline_cache`, `vk_graphics_pipeline`,
`vk_compute_pipeline`, `vk_pipeline_serialization` → Metal versions using `mtl_pso_cache`,
`mtl_shader`, `mtl_spirv_*`, `mtl_tessellation`, `mtl_geometry`)
- The recompiler still emits SPIR-V; SPIRV-Cross turns it into MSL with our direct-slot binding
  model (already validated: all 1,087 cached modules, all 600 pipelines of a Hunter's Dream walk).
- Background compile threads and `shader_compile=wait|skip|balanced` keep their behaviour.
- Persistent cache: our serialized pipeline keys (as now) plus an `MTLBinaryArchive` so
  pipelines warm up without recompiling MSL. A stale or corrupt cache is discarded, never fatal.

**5. Rasterizer** (`vk_rasterizer`, `vk_draw_prep`, `vk_draw_pipe`, `vk_shader_hle`,
`vk_scene_resolution`, `vk_bind_helper`)
- Draw/dispatch preparation is kept; binding goes straight to encoder slots (no push
  descriptors), push constants as bytes, as `mtl_frame` does today.
- Placeholder pipelines for geometry shaders and the interception layer (`mtl_vk_layer`,
  `mtl_shadow`) are deleted: the rasterizer calls Metal itself.

**6. Presenter** (`vk_presenter`, `vk_swapchain`, `host_passes/pp_pass` → Metal)
- `CAMetalLayer` from `SDL_Metal_GetLayer`; `maximumDrawableCount = 2` (today's latency
  setting); `displaySyncEnabled` maps the present modes (Fifo on, Immediate off); fps cap and
  vblank pacing unchanged. Retina drawable size as today. Post-process pass as a render
  pipeline from `post_process.frag`.

**7. Upscalers and motion vectors** (`vk_temporal_upscaler`, `host_passes/fsr_pass`,
`vk_camera_motion`, `vk_object_motion`, `bbport_metalfx.mm`)
- MetalFX: `MTLFXTemporalScaler` on our textures directly (no export, no commit-order
  workaround), encoded into the frame's command buffer.
- TAA, camera and object motion: their compute shaders through the same SPIR-V → MSL path.
- FSR 3.1: a Metal back end for AMD's FidelityFX interface (`FfxInterface`, replacing the SDK's
  `ffx_vk.cpp` role): resources as Metal buffers/textures, AMD's shipped SPIR-V blobs
  compiled to MSL by our compiler, dispatches on our compute encoder. Lives in
  `gpu/shim/ffx_metal/`, built in the arm64 host. The FSR-Vulkan submodule leaves the build.
- FSR 4 / FSR 4.1.1 code (`vk_fsr4`, `fsr411/`) removed from the macOS build.

**8. Overlay and diagnostics**
- ImGui through `imgui_impl_metal` (vendored ImGui has it) on the presenter's command buffer.
- GPU frame capture: `MTLCaptureManager` (`BB_GPU_CAPTURE`). GPU busy time per frame from
  command-buffer `GPUStartTime`/`GPUEndTime` (replaces query pools in `BB_FRAME_STATS`).
- `vk_breadcrumbs` (needs `VK_AMD_buffer_marker`, unavailable on Macs) is deleted.

**9. Build, settings, launcher, packaging**
- `bootstrap_deps.sh`: no MoltenVK, no Vulkan-Loader; Vulkan-Headers (vocabulary), SPIRV-Cross
  and glslang stay. `gpu/CMakeLists.txt`: renderer sources only in the arm64 host library. The
  x86-64 library (today 14 MB, the whole renderer compiled in although split mode never runs it
  there) keeps only what the game side uses in remote mode: the GPU HLE symbol registration,
  the SRT walker and the split shim (`gpu/shim/bbport_split.*`); the plan lists the files.
- `renderer=` in `bbport.ini` is ignored; the in-game menu stops writing it; the launcher's
  Renderer picker is removed.
- `package_app.sh` and CI: no MoltenVK dylib, no ICD json, no `VK_*`/`MVK_*` environment.

## Order of work (milestones)

Each milestone ends with a commit that builds; the game is expected to run again from M4.

| # | Milestone | Done when |
|---|---|---|
| M1 | Device, queue, ticks; Encoder extracted from `mtl_frame` | Unit test: commands encoded through the Encoder, ticks and deferred operations in order, `Wait`/`IsFree` correct (`tests/test_mtl_device.mm`) |
| M2 | Caches on Metal: buffers, arena, staging, textures, views, samplers, transfers, detile, fault parser, blits | Builds; unit test for staging uploads and buffer/texture copies; detile kernels' output equal to the CPU detiler (`tile.h`) on sample tiles |
| M3 | Pipelines and rasterizer on the Encoder; layer and shadow builder deleted | Builds; pipeline-cache check builds every cached pipeline (`--msl-check` extended) |
| M4 | Presenter on `CAMetalLayer` — **first light** | Title screen correct at 60 FPS; save loads |
| M5 | Overlay, MetalFX, TAA, motion vectors; gameplay parity | Test scenes match 0.4.0 (screenshots); 15-minute walks clean; validation layer clean |
| M6 | FSR 3.1 on Metal | FSR picture matches 0.4.0's; FSR FPS ≥ 107 at 1080p Quality |
| M7 | MoltenVK, Vulkan loader, single-process mode, FSR 4 removed; packaging and CI | Success criteria 1–5 |

## Verification

- **Test scenes** (saves in `out/macdata/save-backups/`): Hunter's Dream headstone
  (`loadcrash-20261014-134937`, the head-to-head scene), Central Yharnam lamp with the dog
  cages (`cages-20261016`), Cathedral Ward (`livecopy-20261016`). Scripted with `BB_PAD_FILE`
  (title +30 s, Cross twice, walk and turn), screenshots compared with 0.4.0 side by side.
- **Speed:** the head-to-head protocol (uncapped, warm caches, mean of the last six 5-s windows),
  alternating runs against 0.4.0.
- **Validation:** `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` runs at M4, M5, M7.
- **Unit tests** in `build_macos.sh --test`: the new Metal device/encoder test, staging/copy test
  and detile comparison; the existing runtime, split, launcher and settings suites keep passing.
- The user's live save is never used for tests (copies only).

## Error handling

- A pipeline that fails to compile: logged once, its draws skipped (as today).
- A command buffer that ends in error: logged with its label and the frame number; the frame is
  dropped, rendering continues. Repeated GPU errors (device removed) end the host process,
  which the game side already reports.
- Unsupported texture format: logged, falls back as `host_compatibility` decides today.

## Risks

- **Size.** ~24k lines of renderer plus ~1k Vulkan lines in the caches change; the branch is
  unusable from M2 to M4. Mitigation: M1–M3 are unit-tested before the game runs; `mtl_frame`,
  the PSO builder and the shader compiler are already proven in the game.
- **Ordering of guest uploads** must stay exactly as today (staging, hoisting, host-copy
  waits): the chunked-commit failure on `metal-backend` showed what breaks otherwise.
- **FSR 3.1 back end** is the least known piece (AMD's interface, ~3–5k lines). It is isolated in
  M6 so it cannot block first light or MetalFX/TAA play.
- **Metal limits:** pipelines naming a depth format without a depth attachment (validation
  error on `metal-backend`) need per-attachment variants; 16 GiB private buffer within
  `maxBufferLength` (to check at M2 on 16 GB Macs).
- The 3456×1944 glitch seen on `metal-backend` (backlog) may reappear; watched during M5.

## Later (after 0.5.0)

Pass merging and load/store actions from what is read later; untracked resources with our own
fences; zero-copy guest memory (`newBufferWithBytesNoCopy` over the shared guest mapping);
tessellation without stalls.

## References

- `docs/DESIGN_METAL_BACKEND.md` (the `metal-backend` work this builds on).
- Huax-1839/bloodborne-macos (GPL-2.0-or-later, local copy `~/Developer/others/bloodborne-macos`,
  commit ee3120a): `renderer_metal/mtl_runtime*`, `mtl_scheduler`, `mtl_presenter`,
  `mtl_overlay`, `mtl_runtime_kernels`, and `docs/metal/` design notes are the reference for
  components 1, 3, 6 and 8; code taken from it is credited in file headers and the README.

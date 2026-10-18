# Native Metal back end (plan stage 3)

Branch `metal-backend`. **Status (2026-10-16 morning): the whole game renders through our own Metal
encoding, faster than MoltenVK at 1080p (96.4 vs 94.5 FPS), level at 1440p and with MetalFX,
drawing geometry-shader effects MoltenVK cannot; FSR 3.1, MetalFX and the overlay work.**
Try it: `renderer=metal` in bbport.ini (the in-game menu keeps it), or `BB_METAL_FRAME=1`.

## Why

Measured on the M4 Max in split mode (FINDINGS, "sizing a native Metal back end"):

| | 1440p native | 1080p native |
|---|---|---|
| Frame (uncapped) | ~17.7 ms (55–58 FPS) | ~13.7 ms (71–77 FPS) |
| GPU busy | 14.6 ms (vertex 5.3, fragment 9.3, compute 1.7) | 10.8 ms (vertex 5.3, fragment 4.9, compute 1.7) |
| GPU idle | ~3.1 ms, mostly 4 stalls inside MoltenVK's command buffer | ~2.9 ms |
| Recorder thread | 47% busy, about half of it MoltenVK + Apple's driver | 54% |

What MoltenVK costs us, and what owning the Metal encoding would change:

1. **GPU idle stalls (~2.5–3 ms/frame):** MoltenVK's tessellation emulation, the encoder after an
   image copy, the command-buffer boundary, between compute encoders. Our own encoding orders
   these itself.
2. **Render-pass structure (~409 encoders, ~190 render passes per frame; ~13–16 µs GPU each, plus
   re-binning geometry per pass):** we decide where passes start and end and their load/store
   actions; merge the passes split by copies, uploads and the copy shader; compute in one encoder.
3. **CPU encoding:** half of the recorder thread is MoltenVK's state tracking and command
   translation. Matters most on slower Macs (M1/M2) and is the rival port's limit.

Not changed: the fragment shading cost (same shaders, same MSL from SPIRV-Cross).
Estimate: 1440p ~17.7 → ~13–14 ms, 1080p ~13.7 → ~10–11 ms on the M4 Max.

## Shape: replace the command path, not the whole renderer

The Vulkan renderer is ~25k lines (`renderer_vulkan/`) plus ~10k lines in the texture and buffer
caches that touch Vulkan. Most of the latter is resource management (create, upload, track),
which is not where the time goes. So, in stages:

- **Keep MoltenVK for resources** at first: `VkImage`, `VkBuffer`, memory, samplers, the guest
  arena buffer. `VK_EXT_metal_objects` gives their `MTLTexture`/`MTLBuffer` (MetalFX already
  does this, `gpu/shim/bbport_metalfx.mm`), and `MTLBuffer.gpuAddress` matches the buffer device
  addresses the shaders use.
- **Own the frame's command encoding.** The seam already exists: stage B (`bb:DrawRec`) queues
  deferred commands (`Scheduler::Record`), and `bb:VkRecorder` executes them as `vkCmd*` calls.
  A Metal command path replaces those calls with our own `MTLCommandBuffer` encoders on MoltenVK's
  `MTLCommandQueue` (exported), keeping submission order with MoltenVK's own work.
- **Own the pipelines.** The shader recompiler's SPIR-V goes through SPIRV-Cross (the version
  MoltenVK uses, same options) to MSL, then `MTLRenderPipelineState`/`MTLComputePipelineState`,
  compiled on background threads and kept in an `MTLBinaryArchive`. Bindings: push descriptors map
  directly to `setVertexBuffer`/`setFragmentTexture`… per draw (no descriptor sets to emulate).
- **A switch:** `BB_METAL=1` selects the Metal command path; MoltenVK stays the default and the
  fallback until the Metal path wins on speed and correctness.

## Milestones (each one A/B measured with `prof.sh`-style runs at 1080p and 1440p)

- **M0, interop spike (now):** in the arm64 host, export MoltenVK's queue, an image's texture and a
  buffer; encode our own pass between MoltenVK command buffers, with prefill on; check ordering and
  contents; find MoltenVK's hazard-tracking mode for its resources (tracked: Metal orders our
  encoders for us; untracked: we need fences). Standalone test first (`tests/`), then in the game
  on the present pass.
  *Result (2026-10-15, `tests/test_metal_interop.mm`, in `build_macos.sh --test`):* MoltenVK
  exports its queue, an image's `MTLTexture` and a memory's `MTLBuffer`; a Vulkan clear, our render
  pass + blit, and a Vulkan copy run in queue order, with prefill off and on (2), whether the
  Vulkan copy is recorded before or after our commit (order follows commit). MoltenVK's resources
  are **hazard-tracked** (texture private, buffer shared, `gpuAddress` available), so Metal orders
  our encoders against them without fences at first. *Next:* the same in the game (the present
  pass through our encoder, `BB_METAL=1`).
- **M1, pipelines:** SPIRV-Cross → MSL with our binding model, background compiles, binary archive;
  validated by building every cached pipeline and comparing MSL with MoltenVK's
  (`MVK_CONFIG_SHADER_DUMP_DIR`).
  *Started 2026-10-15:* the SPIR-V → MSL compiler of Huax-1839/bloodborne-macos (GPL-2.0-or-later,
  `renderer_metal/mtl_shader*`, `mtl_spirv_*`; their direct-slot binding model: push constants at
  buffer 0, buffers from 1, textures/samplers from 0, vertex buffers from 30 down; tessellation and
  geometry-kernel variants) with our device wrapper (MoltenVK's device and queue) and fast math as
  MoltenVK uses (`BB_MSL_SAFE_MATH=1`: safe). Built into the arm64 host only, SPIRV-Cross
  `vulkan-sdk-1.4.363.0` from `bootstrap_deps.sh`. `bbgpu-host --msl-check <cache dir>`: **all 1,087
  vertex, fragment and compute modules of our shader cache compile** (2.4 s on 8 threads); the 22
  tessellation modules wait for their variants (M4). Fragment MSL has no `optnone` helpers (our
  2026-10-13 fix carries over); vertex shaders keep them by design.
  *Pipeline states (2026-10-15):* their PSO builder (`mtl_pso_cache`, `vk_to_mtl`,
  `mtl_tessellation` minus the draw encoding) works on our `GraphicsPipelineKey` unchanged (the
  same struct); the blend rules moved into `Vulkan::AttachmentBlendState` and both renderers use
  it (no change to the Vulkan image or speed: 93.7 FPS, same picture). **Shadow check**
  (`BB_METAL_SHADOW=1`, `vk_metal_shadow.h`, `mtl_shadow.mm`): every pipeline the Vulkan path
  creates is built again as a Metal pipeline state from the same key and SPIR-V. Loading the save
  and walking the Hunter's Dream: **~600 pipelines, 525 render + 39 compute built, 0 failures**;
  not yet covered: 22 tessellation, 14 rect lists (MoltenVK emulates both with tessellation; Metal
  path: their `mtl_spirv_rectlist` rewrite and the tessellation variants).
  *Rect lists and tessellation (2026-10-15):* rect lists through their vertex-shader rewrite
  (`mtl_spirv_rectlist`: two triangles per rectangle instead of MoltenVK's tessellation
  emulation), tessellated draws as their three-step form (vertex and control shaders as compute
  kernels, the evaluation shader as the post-tessellation vertex function). Shadow walk: **all
  600 pipelines build: 525 render, 40 compute, 14 rect lists, 21 tessellation, 0 failures, none
  left uncovered.** M1 is complete for the areas walked.
  *Next:* M2, the first draws through our own encoders.
- **M2, draws and dispatches:** graphics draws, compute dispatches, copies and clears of the main
  frame through Metal encoders, our own render-pass boundaries; barriers become our fences.
  Correctness by screenshots on the test routes; then speed.
  *Design (2026-10-15): a command layer.* The renderer records every GPU command as a
  `vk::CommandBuffer` call through vulkan.hpp's dynamic dispatcher (`VULKAN_HPP_DEFAULT_DISPATCHER`,
  filled in `Instance::CreateDevice`). `renderer_metal/mtl_vk_layer.mm` replaces the ~45
  command-buffer and submission functions in it (77 names with extension aliases). For a command
  buffer the Metal path owns, they encode Metal themselves (pipelines from the shadow builder,
  textures and buffers exported from MoltenVK's objects, samplers made from their create infos);
  for every other command buffer they forward to MoltenVK. The renderer stays unchanged, the two
  paths compare image for image, and command buffers move over one kind at a time: the present
  pass (its own command buffer), compute-only work, then the frame. Submission: a submit with
  Metal-owned command buffers is split so MoltenVK's waits come before our command buffer and its
  signals after.
  *Step A (2026-10-15):* the layer forwards and counts (`BB_METAL_TRACE=1`): no cost (95.6 FPS).
  Hunter's Dream, 1080p native, ~95 FPS, per frame: ~820 draws (430 indexed), ~910 pipeline
  binds, push-descriptor sets and push constants (one per draw or dispatch, never deduplicated),
  ~160 render passes, ~157 buffer copies, ~206 `vkCmdPipelineBarrier2`, ~88 dispatches, ~10
  dynamic-state calls per 6 draws, 4 command buffers and 3 submits.
  *Step B (2026-10-16): the first command buffer encoded in Metal.* `BB_METAL_PRESENT=1`: the
  presenter's command buffer is owned by the Metal path (`MetalLayer::Own`); its barriers need
  nothing (tracked resources), `vkCmdClearColorImage` is a render pass with a clear load action,
  `vkCmdBlitImage` (scaled, linear) a fullscreen triangle sampling the source (Metal's blit encoder
  neither scales nor filters). At the submit our command buffer is committed on MoltenVK's queue
  before MoltenVK gets the submit (its copy is empty; semaphores are queue order in MoltenVK).
  16,000+ frames presented through it, 5 commands each, 95.1 FPS (95.0 through MoltenVK), image
  correct. With the overlay showing, the pass stays on MoltenVK (ImGui records Vulkan).
  *Next:* step C, the frame's commands: bound pipelines (from the shadow builder), push
  descriptors and push constants to our slots, render passes, draws, dispatches, copies.
  *Step C (2026-10-16 night): the whole frame in Metal.* `BB_METAL_FRAME=1` owns every command
  buffer at `vkBeginCommandBuffer`. `mtl_frame.mm` encodes render passes (attachments rendered into
  the image's own texture at the view's level/slice), draws (rect lists as the rewritten
  triangles), compute dispatches, copies, fills, updates, clears and blits; push descriptors are
  bound per draw by (set, binding) through each shader's slot map, push constants as bytes;
  buffers come from MoltenVK's `vkGetMTLBufferMVK` plus the device-address offset, views from
  `VK_EXT_metal_objects`, samplers rebuilt at `vkCreateSampler`; buffers whose device address is
  taken are declared on every encoder (`useResources`). Pipelines: the game's from the shadow
  builder (by `VkPipeline`), every other one (post-processing, detiling, copy shader, fault
  buffer, camera motion, blits) translated from its Vulkan create info (`GraphicsCreated`,
  `ComputeCreated`; specialization constants folded into plain constants). 1D images compile as
  2D, as in MoltenVK. **Result: the game renders correctly through it** (Hunter's Dream, same
  image as MoltenVK), 0 unsupported commands, 0 draws without a pipeline; tessellated draws
  (~40/frame) still skipped. First speed: **85.7 FPS vs 94.8 through MoltenVK**, before any
  optimisation (a lock per command, per-bind object lookups, all state set per draw).
  *Later the same night:* unretained-reference command buffers (Metal's completion thread was
  saturated releasing resources, delaying "frame done" 7 ms), a per-thread lookup instead of a
  lock per command, a residency set for the 91 device-address buffers with only the guest arena
  declared per encoder, render encoders declaring the arena read-only (writes come from compute),
  **tessellated draws** (vertex and control shaders as compute kernels, the patch draw in the
  reopened pass) and **geometry-shader draws** (`BB_METAL_GEOMETRY=0` to keep them off): the
  Instance reports a geometry stage on the Metal path, the layer gives those pipelines placeholder
  handles MoltenVK never sees, and Huax-1839's geometry emulation (export and geometry shaders as
  compute kernels, strip draw through a generated pass-through function) draws them. **MoltenVK
  skips these draws; the Metal path renders them** (one such pipeline in the Hunter's Dream).
  Chunked commits (`BB_METAL_CHUNK`) are off: the renderer copies guest data into GPU memory from
  the CPU while recording, so a chunk the GPU started early read data meant for later draws
  (broken geometry while walking; correct with chunking off).
  Hunter's Dream 1080p native, three alternating pairs: **Metal 88.7 FPS (89.9, 87.9, 88.2) vs
  MoltenVK 91.2 (86.5, 91.8, 95.2)**, Metal drawing everything incl. tessellation and geometry
  shaders; walk correct. Known gap: passes whose pipeline names a depth format without a depth
  attachment (Metal validation error, harmless in practice; needs pipeline variants).
  *Parity (2026-10-16, ~03:30):* render encoders open at the first draw (passes without draws
  and without clears cost nothing; ~45/frame); tessellated draws come in runs (78% follow another
  directly): the kernels of a run share one compute pass and the patch draws wait (snapshot of
  the draw state) for one reopened render pass. Descriptor sets and `vkGet*ProcAddr` through the
  layer: **FSR 3.1 and the overlay work**; MetalFX works (depth blits write depth). 6 walks clean;
  pacing better than MoltenVK (0.9 vs 1.4–2.4 late game frames per 5 s; 3–4 vs 8–9 late display
  frames). **1080p native: Metal 95.5/95.7 vs MoltenVK 95.9 FPS**, drawing the geometry-shader
  effect MoltenVK skips; 1440p: 64.9 vs 67.7 (before the tessellation runs); FSR 1080p 99.9;
  MetalFX 1080p 100.3 (vs 107.2).
- **M3, pass merging:** load/store actions from what we know is read later; merge passes split by
  copies/uploads; one compute encoder for consecutive dispatches.
- **M4, tessellation:** Metal tessellation (control stage as a compute kernel, post-tessellation
  vertex function) scheduled without MoltenVK's stalls; rect/quad lists as plain triangles where
  the shader allows.
- **M5, presenter and upscalers:** present through the `CAMetalLayer` directly, MetalFX without the
  commit-order workaround, FSR 3.1 as compute.
- **M6 (optional):** resources created in Metal too; MoltenVK dropped.

## Risks and questions

- MoltenVK's view of resource state: images we write must not be in a state MoltenVK will
  "transition" afterwards (layout changes are no-ops on Metal; check it emits nothing).
- Prefilled command buffers: MoltenVK commits its prefilled command buffer when the Vulkan one
  ends; our command buffers must be committed in order with it (MetalFX met this: it queued behind
  the next prefilled buffer). Owning the whole frame's encoding removes the problem.
- SPIRV-Cross option parity with MoltenVK (the `NoContraction`/precise handling, argument buffers
  off, the same vertex attribute and tessellation conventions).
- The texture cache's copies, blits and tile manager still record Vulkan commands; they move one
  by one (M2/M3), or stay on MoltenVK's command buffers in order.

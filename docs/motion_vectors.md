# Motion vectors: rework after the 2026-09-26 video

Source material: `video_2026-09-26_04-19-53.mp4`, FSR 3.1 Performance,
960×540 render → 1920×1080 output, and `out/dev/t71.log`.
This describes changes in the working tree at the time, not a confirmation of in-game quality.

A shared pass over all G-buffer draws turned out too expensive: Performance ran at about
49 FPS with it and 77–78 FPS without. Object motion vectors are now enabled by default only
for shaders with a pose/bone buffer (640–16384 bytes, excluding the 864-byte shared scene
buffer; 16-byte stride). In a trial run with a narrower 1024-byte threshold this reduced the
processed draws to about 140 per frame, eliminated history overflow and gave about
81–87 FPS. All other draws keep the cheap camera vectors.
`BB_OBJECT_MOTION=0` disables the whole feature, `BB_OBJECT_MOTION_ALL=1` restores the shared
pass for A/B. The alpha channel of the auxiliary target stores depth: if a later regular
draw covers the object, its vector is not used. The quality of the character and weapons in
motion after this selection still needs visual verification.

## Findings

- At the end of the original log, over 600 frames: 257400 G-buffer draws, 256200 declared
  moving, only 30000 got previous positions (~11.7% of all draws).
  This is draw coverage, not a percentage of pixels. The old counter did not distinguish
  the reason for each miss.
- History reserved the size of the whole vertex buffer multiplied by the instance count
  **including firstInstance**. For submeshes of shared buffers these are wasted
  positions; the budget was 2097152 positions per frame.
- History had no dedicated barrier between vertex shader writes/reads; reuse of the
  host parameter ring did not wait for the GPU.
- Disabled writes of all vertices were directed to a single scratch element.
- `CameraMotion::OnDisplayPass` reset Depth() before computing the jitter phases:
  Performance got 8 phases instead of 32. Position reconstruction from depth also
  ignored the viewport offset.

## Changes

- The history range is determined by the min/max of the actual indices, accounting for
  baseVertex and primitive restart. FirstInstance is subtracted in the shader instead of
  consuming memory.
- All eligible direct G-buffer draws are saved, starting from their first appearance.
  The constant-change heuristic was removed: it treated almost all draws as moving
  and missed the start of motion. History is read only when it matches the previous frame.
- The key includes all vertex stream descriptions, the shader, the address and content hash
  of the indices, the vertex range, instances and the ordinal among identical draws.
  A topology change or a skipped frame resets the corresponding history.
- Budget: 4194304 positions/frame; the two arrays together take 128 MiB instead of 64 MiB.
  When memory runs out, camera vectors are used. The log separately reports saves,
  matches, missing history, out-of-memory and invalid ranges.
- Added vertex-to-vertex barriers, a GPU wait before reusing host parameters, coherent host
  memory and independent tracking of the motion image layout. Re-reading the image no longer
  clears the "has vectors" flag.
- Disabled accesses are skipped by branching. Repeated invocations of the same indexed
  vertex use atomic writes of the position components. Positions behind the camera and
  NaN/infinite vectors are not used.
- Performance gets 32 jitter phases after the FSR context is created. Camera vectors are
  reconstructed from unjittered coordinates; object vectors were already derived from clip
  positions before the viewport offset.

## Verification

Build and CPU tests (Linux reference path; the original tree layout had the port under
`native_probe/`):

```bash
cd native_probe
nix-shell shell.nix --run 'bash build.sh && ninja -C out/gpu motion-history-test motion-shader-test && out/gpu/motion-history-test && out/gpu/motion-shader-test out/motion-shaders'
```

`test_motion_history.cpp` checks u16/u32 ranges, restart, negative baseVertex,
firstInstance, repeated draws, topology changes, a skipped frame, budget exhaustion,
overflow and the number of phases. It also passed a run with AddressSanitizer/UBSan.

`test_motion_shaders.cpp` uses the real SPIR-V backend and builds VS/PS with and without
motion vectors. All four results pass `spirv-val --target-env vulkan1.3`.
Repeating this requires `spirv-val` from SPIRV-Tools:

```bash
for shader in out/motion-shaders/*.spv; do
    spirv-val --target-env vulkan1.3 "$shader" || exit
done
```

Build/test logs: `out/dev/motion-validation.log`. The verification environment had no access
to `/dev/dri`: quality on the RX 7800 XT, FPS and coverage after the changes were not yet
checked. Passing SPIR-V validation does not prove the image is correct.

## Next in-game run

From `game_files` (Linux reference path):

```bash
BB_FRAME_STATS=1 BB_TOGGLE_FILE=out/dev/motion-toggles bash native_probe/run.sh 2>&1 | tee native_probe/out/dev/motion-run.log
```

Bit 29 in the runtime toggle file disables only history recording. For a fair performance
comparison, run the game with and without `BB_OBJECT_MOTION=0`: pipeline selection and the
extra target are decided at process start.
After toggling, give FSR a few frames to refresh its history.
Compare the same scene: character/weapon while running, camera turns, combat, still camera.
After ~600 frames check `Object motion:`: `with history`, `capacity skips`, `unmatched`.

Remaining limitations: identical meshes are matched by draw order, which can change when
objects are culled; a change of dynamic vertex buffer addresses loses the match. Indirect
draws, tessellation/geometry shaders, MSAA and passes outside the G-buffer stay on camera
vectors. Transparency/particles, the blurry UI and moving scaling into the renderer are not
addressed by this change. The cost of index scanning and atomic writes needs to be measured
in a real run.

## September 29: weapons and small skeletons (saw-cleaver ghosting)

Video `video_2026-09-29_23-08-26.mp4`: the character stands still, the saw disc spins and
leaves a trail. The `BB_MOTION_SELECT_LOG=1` log showed the cause: the vertex shader's second
buffer is a bone palette of 48 bytes each (3x4), and for weapons/props it is 96–384 bytes
(2–8 bones). The "from 640 bytes" rule excluded these draws, so the disc got only camera
vectors, i.e. it was static as far as FSR was concerned.

Changes:

- `Motion::ClassifyBuffer` (`motion_history.h`): 864 — scene, ≥640 — character skeleton,
  96..639 and a multiple of 48 — small skeleton. Pipelines with any skeleton get the
  motion-vector variant.
- There are about a thousand small skeletons per frame, most of them static map parts.
  So they are gated: the palette hash is compared with the previous frame using a cheap
  key (`History::Moving`) before scanning indices. If it matches, the camera vector is exact
  and nothing is written. If it changed, positions are saved and the object vector is used
  from the next frame. Hashing the 416-byte buffer does not work: it changes with the camera
  for almost everything (verified: history overflow and FPS dropping to ~40).
- Menu: "Show motion vectors (debug)". Red/green — |x|/|y|, blue — the pixel received an
  object vector. `BB_MOTION_SELECT_LOG=1` prints G-buffer shaders with their buffer sizes and
  the decision.

Measurement `out/motion-gated.log`: ~225 saved draws and ~600 "static" ones per frame,
no overflow, 58–80 FPS in gameplay scenes. The disappearance of the trail on the saw still
needs visual confirmation.

# Depth-Adaptive TAA Improvements

**Date:** 2026-10-01  
**Status:** Historical experiment, superseded by a fix to temporal reprojection.

The thresholds and weights described below are no longer used. Lowering the history weight
at distance increased flicker; comparing device depth across different cameras and storing
depth in FP16 made it impossible to tell distant surfaces apart reliably. The current
analysis, changes and verification results were in `docs/TEMPORAL_REVIEW_2026-10-01.md`
(removed from the tree; see git history).

## Problem

The TAA implementation at the time used fixed thresholds for:
1. Disocclusion detection (newly revealed surfaces): `0.0005`
2. Temporal accumulation weight: `0.9` → `0.65` depending on motion

These parameters ignore pixel depth, which causes problems:
- **Distant objects (depth → 1.0)**: temporal instability, flicker
- **Near objects (depth → 0.0)**: excessive blur during fast motion

## Proposed solution: depth-adaptive thresholds

### 1. Depth-Adaptive Disocclusion Threshold

**File:** `gpu/shadps4/video_core/host_shaders/taa.comp`

```glsl
float depthThreshold(float depth) {
    // Near: 0.001, Far (>0.95): 0.0002
    return mix(0.001, 0.0002, smoothstep(0.7, 0.95, depth));
}
```

**Logic:**
- Near surfaces (depth < 0.7): threshold `0.001` — tolerates small differences
- Far surfaces (depth > 0.95): threshold `0.0002` — stricter, fewer false positives
- Smooth transition via `smoothstep(0.7, 0.95, depth)`

**Usage:**
```glsl
float depthDiff = abs(history.a - depth);
float threshold = depthThreshold(depth);
if (depthDiff < threshold && ...) {
    // Accept history
}
```

### 2. Depth-Adaptive Temporal Weight

```glsl
float temporalWeight(float depth, float motionLength) {
    // Base weight decreases for distant objects: 0.9 near, 0.8 far
    float baseWeight = mix(0.9, 0.8, smoothstep(0.7, 0.95, depth));
    // Motion reduces weight more aggressively for distant pixels
    float motionFactor = mix(0.65, 0.5, smoothstep(0.7, 0.95, depth));
    return mix(baseWeight, motionFactor, clamp(motionLength / 16.0, 0.0, 1.0));
}
```

**Logic:**
- **Base weight without motion:**
  - Near: `0.9` (aggressive history accumulation)
  - Far: `0.8` (more conservative, less flicker)

- **During fast motion (motionLength > 16px):**
  - Near: drops to `0.65`
  - Far: drops to `0.5` (even more conservative)

**Usage:**
```glsl
float motionLength = length(motion);
float weight = temporalWeight(depth, motionLength);
result = mix(current, clipped, weight);
```

### 3. Motion Vector Validation (camera_motion.comp)

**File:** `gpu/shadps4/video_core/host_shaders/camera_motion.comp`

```glsl
// Depth-adaptive validation: distant surfaces need tighter depth matching.
float depth_threshold = mix(0.001, 0.0003, smoothstep(0.7, 0.95, depth));
if (object_motion.b > 0.99 && abs(object_motion.a - depth) < depth_threshold &&
    !any(isnan(object_motion.rg)) && !any(isinf(object_motion.rg))) {
    result = object_motion.rg;
}
```

**What changed:**
- Old threshold: fixed `0.001`
- New threshold: `0.001` → `0.0003` for distant surfaces
- Prevents using object motion vectors from the wrong pixels

### 4. Improved debug visualization modes

**File:** `gpu/shadps4/video_core/host_shaders/camera_motion.comp`

The `BB_DEBUG_MOTION=1` modes are now structured:
```glsl
if (mode == 1) {
    // Raw depth visualization
    imageStore(color_img, pixel, vec4(fract(depth * 4.0), depth, depth < 0.0 ? 1.0 : 0.0, 1.0));
} else if (mode == 2) {
    // View-space depth visualization (z / 50)
    const float v = z / 50.0;
    imageStore(color_img, pixel, vec4(fract(v * 4.0), v, v < 0.0 ? 1.0 : 0.0, 1.0));
}
```

## Expected improvements (not achieved, see Status)

### Distant objects
- Less flicker on distant walls and sky
- More stable history during small camera movements
- Fewer false disocclusions from depth precision errors

### Near objects
- Less blur during fast motion (character, weapon)
- More accurate object motion vectors
- Better balance between accumulation and responsiveness

### Transition zone (depth 0.7-0.95)
- Smooth transition between modes
- Avoids hard artifacts at the boundary

## Testing

### Visual check
```bash
# TAA at native resolution
BB_UPSCALER=taa bash run.sh

# Check:
# 1. Flicker on distant walls (Hunter's Dream, upper floors)
# 2. Weapon blur during fast turns
# 3. Stability during slow camera movement
# 4. Disocclusion when walking around corners
```

### Debug modes
```bash
# Depth visualization
BB_DEBUG_MOTION=1 bash run.sh
# Toggle: (1<<21) = raw depth, (1<<22) = view-space z

# Motion vector checks
# Toggle: (1<<20) = blended motion, (1<<23) = reprojection, (1<<24) = difference
```

### A/B comparison
```bash
# Create a file for runtime toggling
echo "0" > /tmp/bb_toggle

# Flip bits in game to compare
# A temporary mechanism for old/new weights would be required
```

## Potential risks

1. **Weights too conservative on distant objects**
   - May increase smearing during fast turns
   - Mitigation: tune `baseWeight` (0.8 → 0.85?)

2. **Depth threshold too strict for object motion**
   - May reject valid vectors
   - Mitigation: `0.0003` → `0.0005` for depth > 0.95

3. **The smoothstep range 0.7-0.95 may be suboptimal**
   - Bloodborne: near=0.05, far=3000
   - Most geometry lies in 0.1-0.9
   - Mitigation: profile the depth distribution in typical scenes

## Next steps (as planned at the time; abandoned)

1. [done] Implement depth-adaptive thresholds
2. [ ] Build and test in game
3. [ ] Collect depth distribution statistics
4. [ ] Tune thresholds based on real data
5. [ ] Add runtime toggling for A/B tests
6. [ ] Document final parameters

## References

- [upscaler.md](upscaler.md) — overall upscaler architecture
- [motion_vectors.md](motion_vectors.md) — motion vectors
- TAA shader: `gpu/shadps4/video_core/host_shaders/taa.comp`
- Camera motion: `gpu/shadps4/video_core/host_shaders/camera_motion.comp`

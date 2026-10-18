// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MetalFX frame interpolation (frame generation) for the Metal-native renderer, macOS 26+.
// BB_FRAME_GENERATION=1 turns it on (off by default). For each game frame N that went through a
// temporal upscaler (MetalFX, FSR 3.1 or TAA: they provide the depth and motion vectors), the
// frame between N-1 and N is generated into the Frame's second texture; the presenter shows it
// before frame N, so a 60 FPS game fills a 120 Hz display. Frames without an upscaler resolve
// (menus, loading screens, upscaler off) are shown as they are and restart the history.
//
// The approach follows Huax-1839/bloodborne-macos's mtl_frame_interpolator.mm (GPL-2.0-or-later):
// standalone interpolator (not combined with the scaler), the UI is part of the colour image
// (no UI texture), the interpolator is created on a thread of its own (first creation can take a
// second; frames meanwhile are not interpolated). Differences: the frame's colour is copied into
// two textures of our own (current/previous, ping-pong), and the depth and motion inputs are
// copied into textures of exactly the scene size (the upscaler's can be larger, with the scene
// in their top-left; macOS 26's interpolator has no content rectangle).
#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
class Encoder;
} // namespace Metal

namespace BbFrameGen {

/// What the temporal upscaler used for a frame (TemporalUpscaler::TakeFrameGenInputs).
struct Inputs {
    void* depth{};  ///< id<MTLTexture>, scene depth; the scene fills its top-left width x height
    void* motion{}; ///< id<MTLTexture>, RG16Float, pixels, previous minus current
    uint32_t width{}, height{}; ///< render (scene) size
    std::array<float, 2> jitter{}; ///< pixels
    float near_plane{}, far_plane{};
    float vertical_fov{}; ///< radians
    bool reset{};         ///< the upscaler restarted its history (camera cut, settings change)
};

/// BB_FRAME_GENERATION=1 (read once).
bool Requested();

/// The work one frame records on the draw scheduler (Encode, inside a Scheduler::Record closure).
struct Job;
void Encode(Metal::Encoder& e, const Job& job);

class Interpolator {
public:
    /// Logs and stays off when this Mac cannot interpolate (macOS < 26, unsupported device).
    explicit Interpolator(Metal::Device& device);
    ~Interpolator();

    Interpolator(const Interpolator&) = delete;
    Interpolator& operator=(const Interpolator&) = delete;

    [[nodiscard]] bool Usable() const;

    /// PrepareFrame, after the frame's colour (`color`, id<MTLTexture>, width x height) is
    /// recorded. `inputs` null: no upscaler resolve this frame (history restarts). `output`
    /// (the Frame's interpolated texture) is (re)made here when its size or format differs.
    /// Returns the work to record (null: nothing), and sets `generated` when that work writes
    /// an interpolated frame into `output`.
    std::shared_ptr<Job> Prepare(void* color, uint32_t width, uint32_t height,
                                 const Inputs* inputs, Metal::Object& output, bool& generated);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace BbFrameGen

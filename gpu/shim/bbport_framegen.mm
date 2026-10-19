// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MetalFX frame interpolation, see bbport_framegen.h. ARC.
#include "bbport_framegen.h"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace BbFrameGen {
namespace {

__attribute__((format(printf, 1, 2))) void Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

id<MTLTexture> Tex(const Metal::Object& object) {
    return (__bridge id<MTLTexture>)object.Get();
}

/// What the interpolator and our textures are made for.
struct Config {
    uint32_t color_format{}, depth_format{};
    uint32_t input_width{}, input_height{};   ///< depth and motion (the scene size)
    uint32_t output_width{}, output_height{}; ///< the frame
    bool operator==(const Config&) const = default;
};

Metal::Object MakeTexture(Metal::Device& device, uint32_t format, uint32_t width, uint32_t height,
                          bool render_target, bool shader_write, const char* label) {
    Metal::TextureDesc desc{};
    desc.pixel_format = format;
    desc.width = width;
    desc.height = height;
    desc.render_target = render_target;
    desc.shader_write = shader_write;
    return Metal::CreateTexture(device, desc, label, nullptr);
}

bool IsDepthFormat(uint32_t format) {
    switch (MTLPixelFormat(format)) {
    case MTLPixelFormatDepth16Unorm:
    case MTLPixelFormatDepth32Float:
    case MTLPixelFormatDepth24Unorm_Stencil8:
    case MTLPixelFormatDepth32Float_Stencil8:
        return true;
    default:
        return false;
    }
}

/// The HUD layer: MetalFX's composited UI mode takes the frame WITHOUT the HUD as its colour (both
/// frames') and the finished frame as its UI texture, and separates the HUD itself. This writes the
/// frame without the HUD: the frame's own pixel, except where the finished UI image differs from its
/// copy taken before the UI draws (3x3 neighbourhood, so anti-aliased edges count), there the copy.
/// params.x: threshold in 1/1024ths. Passing the HUD alone as the UI texture with the composited
/// mode (as before) makes MetalFX draw the generated frames' scene black: every other frame dark,
/// a blinking picture with wrong colours.
constexpr const char* HudMaskSource = R"(
#include <metal_stdlib>
using namespace metal;
kernel void hud_mask(texture2d<float, access::read> frame [[texture(0)]],
                     texture2d<float, access::sample> ui_final [[texture(1)]],
                     texture2d<float, access::sample> ui_scene [[texture(2)]],
                     texture2d<float, access::write> scene [[texture(3)]],
                     constant uint2& params [[buffer(0)]],
                     uint2 gid [[thread_position_in_grid]]) {
    const uint w = scene.get_width(), h = scene.get_height();
    if (gid.x >= w || gid.y >= h) return;
    constexpr sampler nearest(coord::normalized, filter::nearest, address::clamp_to_edge);
    constexpr sampler bilinear(coord::normalized, filter::linear, address::clamp_to_edge);
    const float2 uv = (float2(gid) + 0.5) / float2(w, h);
    const float2 step = max(1.0 / float2(ui_final.get_width(), ui_final.get_height()), 1.0 / float2(w, h));
    float diff = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const float2 p = uv + float2(dx, dy) * step;
            const float3 d = abs(ui_final.sample(nearest, p).rgb - ui_scene.sample(nearest, p).rgb);
            diff = max(diff, max(d.r, max(d.g, d.b)));
        }
    }
    float4 result = frame.read(gid);
    if (diff > float(params.x) / 1024.0) {
        result = float4(ui_scene.sample(bilinear, uv).rgb, 1.0);
    }
    scene.write(result, gid);
}
)";

} // namespace

void CopyTexture(Metal::Encoder& e, void* source, void* destination) {
    e.EndEncoding();
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)e.CommandBuffer();
    @autoreleasepool {
        id<MTLTexture> src = (__bridge id<MTLTexture>)source;
        id<MTLTexture> dst = (__bridge id<MTLTexture>)destination;
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        blit.label = @"Frame generation UI scene copy";
        [blit copyFromTexture:src
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(std::min(src.width, dst.width),
                                          std::min(src.height, dst.height), 1)
                    toTexture:dst
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
    }
    e.KeepAlive(Metal::Object::Retain(source));
    e.KeepAlive(Metal::Object::Retain(destination));
}

bool Requested() {
    static const bool requested = [] {
        const char* env = std::getenv("BB_FRAME_GENERATION");
        return env && std::strcmp(env, "0") != 0 && *env;
    }();
    return requested;
}

struct Job {
    Metal::Object interpolator; ///< id<MTLFXFrameInterpolator>; empty: copy only (no frame)
    Metal::Object color;        ///< the frame (source of the copy)
    Metal::Object current, previous;
    Metal::Object depth_source, motion_source;
    Metal::Object depth, motion; ///< scene-size copies
    Metal::Object output;
    Metal::Object hud_pipeline;      ///< id<MTLComputePipelineState> (hud_mask)
    Metal::Object hud;               ///< the HUD layer (uiTexture), frame size
    Metal::Object ui_final, ui_scene; ///< empty: no UI information this frame
    uint32_t hud_threshold{6};
    uint32_t width{}, height{};  ///< scene size (the copied region)
    float delta_time{}, near_plane{}, far_plane{}, fov_degrees{}, aspect{};
    std::array<float, 2> jitter{};
    bool reset{};
};

void Encode(Metal::Encoder& e, const Job& job) {
    e.EndEncoding();
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)e.CommandBuffer();
    @autoreleasepool {
        const bool has_ui = job.hud_pipeline && job.ui_final && job.ui_scene;
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        blit.label = @"Frame generation inputs";
        id<MTLTexture> color = Tex(job.color);
        if (!has_ui) [blit copyFromTexture:color
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(color.width, color.height, 1)
                    toTexture:Tex(job.current)
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        if (job.interpolator) {
            const MTLSize scene = MTLSizeMake(job.width, job.height, 1);
            [blit copyFromTexture:Tex(job.depth_source)
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:scene
                        toTexture:Tex(job.depth)
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit copyFromTexture:Tex(job.motion_source)
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:scene
                        toTexture:Tex(job.motion)
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        [blit endEncoding];

        if (has_ui) {
            // This frame's colour (the next frame's previous image) without the HUD.
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            ce.label = @"Frame generation colour without the HUD";
            [ce setComputePipelineState:(__bridge id<MTLComputePipelineState>)job.hud_pipeline.Get()];
            [ce setTexture:color atIndex:0];
            [ce setTexture:Tex(job.ui_final) atIndex:1];
            [ce setTexture:Tex(job.ui_scene) atIndex:2];
            [ce setTexture:Tex(job.current) atIndex:3];
            const uint32_t params[2] = {job.hud_threshold, 0u};
            [ce setBytes:params length:sizeof(params) atIndex:0];
            id<MTLTexture> current = Tex(job.current);
            [ce dispatchThreadgroups:MTLSizeMake((current.width + 15) / 16, (current.height + 15) / 16, 1)
                threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            [ce endEncoding];
        }

        if (job.interpolator) {
            if (@available(macOS 26.0, *)) {
                id<MTLFXFrameInterpolator> fi =
                    (__bridge id<MTLFXFrameInterpolator>)job.interpolator.Get();
                fi.colorTexture = Tex(job.current);
                fi.prevColorTexture = Tex(job.previous);
                fi.depthTexture = Tex(job.depth);
                fi.motionTexture = Tex(job.motion);
                fi.outputTexture = Tex(job.output);
                // The finished frame is the UI over the colour: MetalFX separates the HUD from
                // it, keeps it out of the interpolation and draws it on top unchanged.
                fi.uiTexture = has_ui ? color : nil;
                fi.uiTextureComposited = has_ui ? YES : NO;
                fi.deltaTime = job.delta_time;
                fi.nearPlane = job.near_plane;
                fi.farPlane = job.far_plane;
                fi.fieldOfView = job.fov_degrees;
                fi.aspectRatio = job.aspect;
                fi.jitterOffsetX = job.jitter[0];
                fi.jitterOffsetY = job.jitter[1];
                fi.motionVectorScaleX = 1.0f; // pixels, previous minus current (as the scaler's)
                fi.motionVectorScaleY = 1.0f;
                fi.depthReversed = NO;
                fi.shouldResetHistory = job.reset;
                [fi encodeToCommandBuffer:cb];
            }
        }
    }
    // Command buffers do not retain what MetalFX and the blit use.
    for (const Metal::Object* object : {&job.interpolator, &job.color, &job.current, &job.previous,
                                        &job.depth_source, &job.motion_source, &job.depth,
                                        &job.motion, &job.output, &job.hud_pipeline, &job.hud,
                                        &job.ui_final, &job.ui_scene}) {
        if (*object) {
            e.KeepAlive(Metal::Object::Retain(object->Get()));
        }
    }
    static std::atomic<bool> first{false};
    if (job.interpolator && !first.exchange(true)) {
        Log("Frame generation: first frame generated\n");
    }
}

struct Interpolator::Impl {
    Metal::Device& device;
    bool usable = false;
    bool failed = false;
    Config config;
    Metal::Object interpolator;
    Metal::Object frames[2]; ///< ping-pong: current and previous colour
    uint32_t current = 0;
    bool previous_valid = false;
    bool last_generated = false; ///< the previous frame was interpolated (history continues)
    bool previous_ui = false;    ///< the previous colour is without its HUD (the HUD layer)
    Metal::Object depth, motion;
    Metal::Object hud;          ///< the HUD layer, frame size, colour format
    Metal::Object hud_pipeline; ///< id<MTLComputePipelineState>; empty: no HUD layer (logged)
    bool hud_tried = false;
    uint32_t hud_threshold = 6;
    bool usage_checked = false;
    std::chrono::steady_clock::time_point last_frame{};

    // The creation thread writes `created`/`creation_done` under `mutex`; Poll takes them.
    std::thread creator;
    std::mutex mutex;
    Metal::Object created;
    bool creation_done = false;

    explicit Impl(Metal::Device& device_) : device{device_} {}

    void StartCreation() {
        const Config target = config;
        id<MTLDevice> mtl = (__bridge id<MTLDevice>)device.NativeDevice();
        creation_done = false;
        created = {};
        creator = std::thread([this, mtl, target] {
            const auto start = std::chrono::steady_clock::now();
            Metal::Object result;
            if (@available(macOS 26.0, *)) {
                @autoreleasepool {
                    MTLFXFrameInterpolatorDescriptor* d = [MTLFXFrameInterpolatorDescriptor new];
                    d.colorTextureFormat = MTLPixelFormat(target.color_format);
                    d.outputTextureFormat = MTLPixelFormat(target.color_format);
                    d.depthTextureFormat = MTLPixelFormat(target.depth_format);
                    d.motionTextureFormat = MTLPixelFormatRG16Float;
                    d.uiTextureFormat = MTLPixelFormat(target.color_format);
                    d.inputWidth = target.input_width;
                    d.inputHeight = target.input_height;
                    d.outputWidth = target.output_width;
                    d.outputHeight = target.output_height;
                    id<MTLFXFrameInterpolator> fi = [d newFrameInterpolatorWithDevice:mtl];
                    if (fi) {
                        result = Metal::Object::Adopt((__bridge_retained void*)fi);
                    }
                }
            }
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
            Log("Frame generation: MetalFX interpolator %ux%u (scene %ux%u, colour format %u, "
                "depth format %u) %s in %.0f ms\n",
                target.output_width, target.output_height, target.input_width,
                target.input_height, target.color_format, target.depth_format,
                result ? "ready" : "FAILED", ms);
            std::lock_guard lock{mutex};
            created = std::move(result);
            creation_done = true;
        });
    }

    void Poll() {
        if (!creator.joinable()) {
            return;
        }
        {
            std::lock_guard lock{mutex};
            if (!creation_done) {
                return;
            }
        }
        creator.join();
        interpolator = std::move(created);
        if (!interpolator) {
            failed = true;
        }
    }

    void MakeHudPipeline() {
        hud_tried = true;
        if (const char* env = std::getenv("BB_FRAME_GENERATION_HUD"); env && !std::strcmp(env, "0")) {
            Log("Frame generation: HUD layer off (BB_FRAME_GENERATION_HUD=0)\n");
            return;
        }
        if (const char* env = std::getenv("BB_FRAME_GENERATION_HUD_THRESHOLD")) {
            hud_threshold = uint32_t(std::clamp(std::atoi(env), 1, 512));
        }
        id<MTLDevice> mtl = (__bridge id<MTLDevice>)device.NativeDevice();
        NSError* error = nil;
        id<MTLLibrary> library = [mtl newLibraryWithSource:@(HudMaskSource) options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"hud_mask"];
        id<MTLComputePipelineState> pipeline =
            function ? [mtl newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!pipeline) {
            Log("Frame generation: HUD layer shader failed (%s): the HUD is interpolated\n",
                error ? error.localizedDescription.UTF8String : "no function");
            return;
        }
        hud_pipeline = Metal::Object::Adopt((__bridge_retained void*)pipeline);
        Log("Frame generation: HUD kept steady (MetalFX UI layer, threshold %u/1024)\n",
            hud_threshold);
    }

    bool CheckUsage(void* output) {
        if (@available(macOS 26.0, *)) {
            id<MTLFXFrameInterpolator> fi = (__bridge id<MTLFXFrameInterpolator>)interpolator.Get();
            const auto has = [](id<MTLTexture> t, MTLTextureUsage usage) {
                return (t.usage & usage) == usage;
            };
            if (has(Tex(frames[0]), fi.colorTextureUsage) && has(Tex(depth), fi.depthTextureUsage) &&
                has(Tex(motion), fi.motionTextureUsage) &&
                (!hud || has(Tex(hud), fi.uiTextureUsage)) &&
                has((__bridge id<MTLTexture>)output, fi.outputTextureUsage)) {
                return true;
            }
            Log("Frame generation: a texture lacks the usage MetalFX needs (colour %lx, depth %lx, "
                "motion %lx, output %lx): off\n",
                (unsigned long)fi.colorTextureUsage, (unsigned long)fi.depthTextureUsage,
                (unsigned long)fi.motionTextureUsage, (unsigned long)fi.outputTextureUsage);
        }
        return false;
    }
};

Interpolator::Interpolator(Metal::Device& device) : impl{std::make_unique<Impl>(device)} {
    if (@available(macOS 26.0, *)) {
        id<MTLDevice> mtl = (__bridge id<MTLDevice>)device.NativeDevice();
        if ([MTLFXFrameInterpolatorDescriptor supportsDevice:mtl]) {
            impl->usable = true;
            Log("Frame generation: on (MetalFX frame interpolation; needs an upscaler: MetalFX, "
                "FSR 3.1 or TAA)\n");
            return;
        }
        Log("Frame generation: this Mac does not support MetalFX frame interpolation: off\n");
        return;
    }
    Log("Frame generation: needs macOS 26: off\n");
}

Interpolator::~Interpolator() {
    if (impl->creator.joinable()) {
        impl->creator.join();
    }
}

bool Interpolator::Usable() const {
    return impl->usable && !impl->failed;
}

std::shared_ptr<Job> Interpolator::Prepare(void* color, uint32_t width, uint32_t height,
                                           const Inputs* in, Metal::Object& output,
                                           bool& generated) {
    generated = false;
    Impl& s = *impl;
    const auto now = std::chrono::steady_clock::now();
    float delta = std::chrono::duration<float>(now - s.last_frame).count();
    s.last_frame = now;
    if (!Usable() || !color || !width || !height) {
        return nullptr;
    }
    if (!in || !in->depth || !in->motion || !in->width || !in->height) {
        // No upscaler resolve (menu, loading screen, upscaler off): shown as is.
        s.previous_valid = false;
        s.last_generated = false;
        return nullptr;
    }
    id<MTLTexture> depth_source = (__bridge id<MTLTexture>)in->depth;
    id<MTLTexture> motion_source = (__bridge id<MTLTexture>)in->motion;
    if (depth_source.width < in->width || depth_source.height < in->height ||
        motion_source.width < in->width || motion_source.height < in->height ||
        motion_source.pixelFormat != MTLPixelFormatRG16Float ||
        !IsDepthFormat(uint32_t(depth_source.pixelFormat))) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            Log("Frame generation: unexpected inputs (depth %lux%lu format %lu, motion %lux%lu "
                "format %lu, scene %ux%u): frames not generated\n",
                (unsigned long)depth_source.width, (unsigned long)depth_source.height,
                (unsigned long)depth_source.pixelFormat, (unsigned long)motion_source.width,
                (unsigned long)motion_source.height, (unsigned long)motion_source.pixelFormat,
                in->width, in->height);
        }
        s.previous_valid = false;
        return nullptr;
    }

    const Config config{Metal::TexturePixelFormat(color), uint32_t(depth_source.pixelFormat),
                        in->width, in->height, width, height};
    if (!(config == s.config) || !s.frames[0]) {
        // A new size or format (or the first frame): everything again, the interpolator on a
        // thread of its own. Earlier frames' commands keep their textures alive (KeepAlive).
        if (s.creator.joinable()) {
            s.creator.join();
        }
        s.config = config;
        s.interpolator = {};
        s.created = {};
        s.usage_checked = false;
        s.previous_valid = false;
        for (auto& frame : s.frames) {
            frame = MakeTexture(s.device, config.color_format, width, height, true, true,
                                "Frame generation colour");
        }
        s.depth = MakeTexture(s.device, config.depth_format, in->width, in->height, true, false,
                              "Frame generation depth");
        s.motion = MakeTexture(s.device, MTLPixelFormatRG16Float, in->width, in->height, true,
                               true, "Frame generation motion");
        s.hud = MakeTexture(s.device, config.color_format, width, height, true, true,
                            "Frame generation HUD layer");
        if (!s.hud_tried) {
            s.MakeHudPipeline();
        }
        if (!s.frames[0] || !s.frames[1] || !s.depth || !s.motion) {
            Log("Frame generation: cannot allocate its textures: off\n");
            s.failed = true;
            return nullptr;
        }
        s.StartCreation();
    }
    if (!output || Metal::TexturePixelFormat(output.Get()) != config.color_format ||
        Tex(output).width != width || Tex(output).height != height) {
        output = MakeTexture(s.device, config.color_format, width, height, true, true,
                             "Frame generation output");
        if (!output) {
            Log("Frame generation: cannot allocate the output texture: off\n");
            s.failed = true;
            return nullptr;
        }
    }
    s.Poll();
    if (s.failed) {
        return nullptr;
    }
    if (s.interpolator && !s.usage_checked) {
        s.usage_checked = true;
        if (!s.CheckUsage(output.Get())) {
            s.failed = true;
            return nullptr;
        }
    }

    auto job = std::make_shared<Job>();
    job->color = Metal::Object::Retain(color);
    job->current = Metal::Object::Retain(s.frames[s.current].Get());
    job->previous = Metal::Object::Retain(s.frames[s.current ^ 1].Get());
    // The HUD layer needs this frame's UI images; the previous colour must be of the same kind
    // (with or without the HUD), else this frame only starts the history again.
    const bool has_ui = s.hud_pipeline && in->ui_final && in->ui_scene;
    if (has_ui) {
        job->hud_pipeline = Metal::Object::Retain(s.hud_pipeline.Get());
        job->hud_threshold = s.hud_threshold;
        job->ui_final = Metal::Object::Retain(in->ui_final);
        job->ui_scene = Metal::Object::Retain(in->ui_scene);
    }
    if (s.interpolator && s.previous_valid && s.previous_ui == has_ui) {
        job->interpolator = Metal::Object::Retain(s.interpolator.Get());
        job->depth_source = Metal::Object::Retain(in->depth);
        job->motion_source = Metal::Object::Retain(in->motion);
        job->depth = Metal::Object::Retain(s.depth.Get());
        job->motion = Metal::Object::Retain(s.motion.Get());
        job->output = Metal::Object::Retain(output.Get());
        job->width = in->width;
        job->height = in->height;
        job->delta_time = std::clamp(delta, 1.0f / 240.0f, 1.0f / 15.0f);
        job->near_plane = in->near_plane;
        job->far_plane = in->far_plane;
        job->fov_degrees = in->vertical_fov * 57.29577951f;
        job->aspect = float(in->width) / float(in->height);
        job->jitter = in->jitter;
        job->reset = in->reset || !s.last_generated;
        generated = true;
    }
    s.last_generated = generated;
    // This frame's colour is the next frame's previous image.
    s.current ^= 1;
    s.previous_valid = true;
    s.previous_ui = has_ui;
    return job;
}

} // namespace BbFrameGen

// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include <algorithm>
#include <mutex>
#include <vector>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

namespace {

u32 TexturePixelFormat(vk::Format format) {
    const u32 pixel_format = Metal::VkToMtl::PixelFormat(format);
    if (pixel_format != 0) {
        return pixel_format;
    }
    static std::mutex mutex;
    static std::vector<vk::Format> logged;
    std::scoped_lock lock{mutex};
    if (std::ranges::find(logged, format) == logged.end()) {
        logged.push_back(format);
        LOG_ERROR(Render_Vulkan, "image format {} has no Metal texture format, using RGBA8Unorm",
                  vk::to_string(format));
    }
    DEBUG_ASSERT_MSG(false, "image format without a Metal texture format");
    // The stand-in for formats Metal has no texture format for.
    return Metal::VkToMtl::PixelFormat(vk::Format::eR8G8B8A8Unorm);
}

Metal::TextureType TextureTypeOf(const ImageInfo& info, u32 samples) {
    if (info.type == AmdGpu::ImageType::Color3D) {
        return Metal::TextureType::Tex3D;
    }
    // 1D images are 2D textures one texel high (as the shader side samples them, see
    // mtl_shader.mm texture_1D_as_2D); cubes are already 2D arrays (amdgpu/resource.h).
    const bool array = info.type == AmdGpu::ImageType::Color1DArray ||
                       info.type == AmdGpu::ImageType::Color2DArray ||
                       info.type == AmdGpu::ImageType::Color2DMsaaArray ||
                       info.resources.layers > 1;
    if (samples > 1) {
        return array ? Metal::TextureType::Tex2DMultisampleArray
                     : Metal::TextureType::Tex2DMultisample;
    }
    return array ? Metal::TextureType::Tex2DArray : Metal::TextureType::Tex2D;
}

/// The highest supported count at or below `num_samples` (halving, at least 1).
u32 ClampSamples(u32 num_samples, u32 supported_mask) {
    u32 samples = std::max(num_samples, 1u);
    while (samples > 1 && !(supported_mask & samples)) {
        samples /= 2;
    }
    return samples;
}

} // namespace

Image::Image(Metal::Device& device_, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : guest_begin{info_.guest_address}, guest_end{info_.guest_address + info_.guest_size},
      info{info_}, runtime{&runtime_}, slot_image_views{&slot_image_views_}, device{&device_} {
    BbStats::Timer timer{BbStats::t_image_create};
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }

    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    has_depth = info.props.is_depth;
    has_stencil = info.props.is_depth && info.props.has_stencil;

    supported_samples_mask = 0;
    for (const u32 samples : {1u, 2u, 4u, 8u}) {
        if (samples == 1 || Metal::SupportsSampleCount(device_, samples)) {
            supported_samples_mask |= samples;
        }
    }

    // Usage: block (BC) images are only sampled and filled by buffer copies. Metal cannot write
    // compressed textures, not even through uncompressed views (the 2026-10-11 BC rule). Every
    // other image can be rendered to, written by compute (colour, single-sample: clears and
    // copies by compute need no re-creation) and viewed with another pixel format (Vulkan's
    // eMutableFormat).
    texture_desc.pixel_format = TexturePixelFormat(info.pixel_format);
    texture_desc.width = info.size.width;
    texture_desc.height = info.type == AmdGpu::ImageType::Color1D ||
                                  info.type == AmdGpu::ImageType::Color1DArray
                              ? 1
                              : info.size.height;
    texture_desc.depth = info.type == AmdGpu::ImageType::Color3D ? info.size.depth : 1;
    texture_desc.levels = info.resources.levels;
    texture_desc.layers = info.type == AmdGpu::ImageType::Color3D ? 1 : info.resources.layers;
    texture_desc.render_target = !info.props.is_block;
    texture_desc.shader_write = !info.props.is_block && !info.props.is_depth;
    texture_desc.format_views = !info.props.is_block;

    backing = &CreateBacking(info.num_samples);
}

Image::~Image() = default;

Image::BackingImage& Image::CreateBacking(u32 num_samples) {
    const u32 samples = ClampSamples(num_samples, supported_samples_mask);
    Metal::TextureDesc desc = texture_desc;
    desc.samples = samples;
    desc.type = TextureTypeOf(info, samples);
    // Multisample textures can be neither written by shaders nor mipmapped.
    desc.shader_write = texture_desc.shader_write && samples == 1;
    if (samples > 1) {
        desc.levels = 1;
    }

    auto& new_backing = backing_images.emplace_back();
    new_backing.num_samples = num_samples;
    new_backing.desc = desc;
    const std::string label = fmt::format(
        "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width, info.size.height,
        info.size.depth, AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
        info.guest_address, info.guest_size, info.resources.layers, info.resources.levels,
        num_samples);
    new_backing.texture =
        Metal::CreateTexture(*device, desc, label, &new_backing.allocated_size);
    ASSERT_MSG(new_backing.texture, "Failed creating texture for {}", label);
    return new_backing;
}

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    if (backing->last_view < view_infos.size() && view_infos[backing->last_view] == view_info) {
        return (*slot_image_views)[backing->image_view_ids[backing->last_view]];
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        backing->last_view = static_cast<u32>(std::distance(view_infos.begin(), it));
        const auto view_id = backing->image_view_ids[backing->last_view];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

} // namespace VideoCore

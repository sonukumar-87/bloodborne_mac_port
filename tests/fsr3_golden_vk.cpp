// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: one-shot tool (Metal-native M6 Task 3) that runs FSR 3.1 through the Vulkan path the
// game used before the Metal renderer (FSR-Vulkan's ffx-vulkan::portable: AMD's SDK 1.1.4 host code
// and ffx_vk.cpp) on MoltenVK, headless, over the synthetic frames of fsr3_synthetic.h, and writes
// the outputs of the golden frames as raw RGBA16F into a directory (tests/data/fsr3). The Metal back
// end's test (Fsr3MatchesVulkan) compares against them. Built only with -DBB_FSR3_GOLDEN=ON:
//
//   cmake -S gpu -B out/gpu-arm64 -DBB_FSR3_GOLDEN=ON && ninja -C out/gpu-arm64 fsr3-golden-vk
//   out/gpu-arm64/fsr3-golden-vk tests/data/fsr3
//
// The loader finds MoltenVK through VK_DRIVER_FILES (set to the arm64 deps ICD when unset).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "ffx_vk_portable.h"
#include "fsr3_synthetic.h"

#ifndef BB_MOLTENVK_ICD
#define BB_MOLTENVK_ICD ""
#endif

namespace {

using namespace Fsr3Synthetic;

[[noreturn]] void Die(const char* what, VkResult result = VK_SUCCESS) {
    std::fprintf(stderr, "fsr3-golden-vk: %s (VkResult %d)\n", what, int(result));
    std::exit(1);
}

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        Die(what, result);
    }
}

struct Gpu {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkCommandPool pool{};
    bool fp16{};
    VkPhysicalDeviceMemoryProperties memory{};
};

uint32_t MemoryType(const Gpu& gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < gpu.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (gpu.memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    Die("no memory type");
}

Gpu CreateGpu() {
    Gpu gpu;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "fsr3-golden-vk";
    app.apiVersion = VK_API_VERSION_1_2;
    const char* instance_extensions[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
                                         VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = instance_extensions;
    Check(vkCreateInstance(&instance_info, nullptr, &gpu.instance), "vkCreateInstance");

    uint32_t count = 1;
    VkResult result = vkEnumeratePhysicalDevices(gpu.instance, &count, &gpu.physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
        Die("no Vulkan device", result);
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu.physical, &properties);
    std::printf("device: %s (Vulkan %u.%u)\n", properties.deviceName,
                VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, family_props.data());
    gpu.family = UINT32_MAX;
    for (uint32_t i = 0; i < families; ++i) {
        if (family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            gpu.family = i;
            break;
        }
    }
    if (gpu.family == UINT32_MAX) {
        Die("no compute queue");
    }

    // The features the game's device enabled for FSR 3 (vk_instance.cpp): fp16 and 16-bit
    // storage where present, format-less storage writes (FSR's output UAV).
    VkPhysicalDeviceVulkan11Features supported11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    supported11.pNext = &supported12;
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &supported11;
    vkGetPhysicalDeviceFeatures2(gpu.physical, &supported);
    if (!supported.features.shaderStorageImageWriteWithoutFormat) {
        Die("no shaderStorageImageWriteWithoutFormat");
    }
    VkPhysicalDeviceVulkan11Features enable11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    enable11.storageBuffer16BitAccess = supported11.storageBuffer16BitAccess;
    enable11.uniformAndStorageBuffer16BitAccess = supported11.uniformAndStorageBuffer16BitAccess;
    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enable12.shaderFloat16 = supported12.shaderFloat16;
    enable11.pNext = &enable12;
    VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enable.pNext = &enable11;
    enable.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enable.features.shaderStorageImageReadWithoutFormat =
        supported.features.shaderStorageImageReadWithoutFormat;
    enable.features.shaderInt16 = supported.features.shaderInt16;
    // ffx_vk.cpp reports fp16 when the device lists VK_KHR_shader_float16_int8 and the owner
    // enabled shaderFloat16.
    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu.physical, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(gpu.physical, nullptr, &extension_count,
                                         extensions.data());
    bool float16_extension = false;
    for (const VkExtensionProperties& extension : extensions) {
        float16_extension |=
            std::strcmp(extension.extensionName, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) == 0;
    }
    gpu.fp16 = supported12.shaderFloat16 == VK_TRUE && float16_extension;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = gpu.family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* device_extensions[] = {"VK_KHR_portability_subset"};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &enable;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    Check(vkCreateDevice(gpu.physical, &device_info, nullptr, &gpu.device), "vkCreateDevice");
    vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = gpu.family;
    Check(vkCreateCommandPool(gpu.device, &pool_info, nullptr, &gpu.pool), "vkCreateCommandPool");
    return gpu;
}

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
};

Buffer CreateHostBuffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer out;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    Check(vkCreateBuffer(gpu.device, &info, nullptr, &out.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(gpu.device, out.buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        MemoryType(gpu, requirements.memoryTypeBits,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(gpu.device, &allocate, nullptr, &out.memory), "vkAllocateMemory");
    Check(vkBindBufferMemory(gpu.device, out.buffer, out.memory, 0), "vkBindBufferMemory");
    Check(vkMapMemory(gpu.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "vkMapMemory");
    return out;
}

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkFormat format{};
    uint32_t width{}, height{};
    VkImageUsageFlags usage{};
};

Image CreateImage(const Gpu& gpu, VkFormat format, uint32_t width, uint32_t height) {
    Image out;
    out.format = format;
    out.width = width;
    out.height = height;
    out.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = out.usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(gpu.device, &info, nullptr, &out.image), "vkCreateImage");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(gpu.device, out.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        MemoryType(gpu, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(gpu.device, &allocate, nullptr, &out.memory), "vkAllocateMemory");
    Check(vkBindImageMemory(gpu.device, out.image, out.memory, 0), "vkBindImageMemory");
    return out;
}

FfxVkPortableImage Describe(const Image& image, FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image.image;
    out.format = image.format;
    out.extent = {image.width, image.height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = image.usage;
    out.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    out.state = state;
    return out;
}

void Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src,
                VkPipelineStageFlags dst) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Fills `image` (any layout, contents discarded) from `staging` and leaves it in GENERAL for
// shader reads (FFX_RESOURCE_STATE_GENERIC_READ).
void Upload(VkCommandBuffer cmd, const Image& image, const Buffer& staging) {
    Transition(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image.width, image.height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
    Transition(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

bool WriteFile(const std::string& path, const void* data, size_t size) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    const bool ok = std::fwrite(data, 1, size, file) == size;
    return std::fclose(file) == 0 && ok;
}

void RunCase(const Gpu& gpu, const Fsr3Case& c, const std::string& out_dir) {
    const uint32_t w = c.render_width, h = c.render_height;
    const uint32_t ow = c.output_width, oh = c.output_height;

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = gpu.instance;
    device_info.physicalDevice = gpu.physical;
    device_info.device = gpu.device;
    device_info.getDeviceProcAddr = vkGetDeviceProcAddr;
    device_info.queue = gpu.queue;
    device_info.queueFamilyIndex = gpu.family;
    device_info.shaderFloat16Enabled = gpu.fp16 ? VK_TRUE : VK_FALSE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;
    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.flags = c.flags;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    FfxVkPortableUpscaleContext* context = nullptr;
    const FfxVkPortableResult created =
        ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context);
    if (created != FFX_VK_PORTABLE_OK) {
        std::fprintf(stderr, "fsr3-golden-vk: %s: context creation failed (%d)\n", c.name,
                     int(created));
        std::exit(1);
    }

    const Image color = CreateImage(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, w, h);
    const Image depth = CreateImage(gpu, VK_FORMAT_R32_SFLOAT, w, h);
    const Image motion = CreateImage(gpu, VK_FORMAT_R16G16_SFLOAT, w, h);
    const Image output = CreateImage(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh);
    const Buffer color_staging =
        CreateHostBuffer(gpu, VkDeviceSize(w) * h * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const Buffer depth_staging =
        CreateHostBuffer(gpu, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const Buffer motion_staging =
        CreateHostBuffer(gpu, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const size_t output_bytes = size_t(ow) * oh * 8;
    const Buffer readback = CreateHostBuffer(gpu, output_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = gpu.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    Check(vkAllocateCommandBuffers(gpu.device, &allocate, &cmd), "vkAllocateCommandBuffers");

    for (uint32_t frame_index = 0; frame_index < kFrames; ++frame_index) {
        const Fsr3Frame frame = MakeFrame(frame_index, w, h);
        std::memcpy(color_staging.mapped, frame.color.data(), frame.color.size() * 2);
        std::memcpy(depth_staging.mapped, frame.depth.data(), frame.depth.size() * 4);
        std::memcpy(motion_staging.mapped, frame.motion.data(), frame.motion.size() * 2);

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        Upload(cmd, color, color_staging);
        Upload(cmd, depth, depth_staging);
        Upload(cmd, motion, motion_staging);
        if (frame_index == 0) {
            Transition(cmd, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }

        FfxVkPortableUpscaleDispatchInfo info{};
        info.structSize = sizeof(info);
        info.commandBuffer = cmd;
        info.color = Describe(color, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.depth = Describe(depth, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.motionVectors = Describe(motion, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.output = Describe(output, FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        info.exposure.structSize = sizeof(info.exposure);
        info.reactiveMask.structSize = sizeof(info.reactiveMask);
        info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
        info.jitterOffset = {frame.jitter_x, frame.jitter_y};
        info.motionVectorScale = {kMotionScale, kMotionScale};
        info.renderSize = {w, h};
        info.outputSize = {ow, oh};
        info.frameTimeMilliseconds = kFrameTimeMs;
        info.preExposure = kPreExposure;
        info.cameraNear = kCameraNear;
        info.cameraFar = kCameraFar;
        info.cameraVerticalFovRadians = kCameraFovY;
        info.viewSpaceToMeters = kViewSpaceToMeters;
        info.sharpness = c.sharpness;
        info.enableSharpening = c.sharpen ? VK_TRUE : VK_FALSE;
        info.reset = frame.reset ? VK_TRUE : VK_FALSE;
        info.frameId = frame_index;
        const FfxVkPortableResult dispatched =
            ffxVkPortableUpscaleContextRecordDispatch(context, &info);
        if (dispatched != FFX_VK_PORTABLE_OK) {
            std::fprintf(stderr, "fsr3-golden-vk: %s frame %u: dispatch failed (%d)\n", c.name,
                         frame_index, int(dispatched));
            std::exit(1);
        }

        bool golden = false;
        for (const uint32_t g : kGoldenFrames) {
            golden |= g == frame_index;
        }
        // FFX leaves the output in GENERAL (UNORDERED_ACCESS).
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        if (golden) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {ow, oh, 1};
            vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1,
                                   &region);
        }
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Check(vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        Check(vkQueueWaitIdle(gpu.queue), "vkQueueWaitIdle");

        if (golden) {
            const std::string path =
                out_dir + "/" + c.name + "_f" + std::to_string(frame_index) + ".bin";
            if (!WriteFile(path, readback.mapped, output_bytes)) {
                std::fprintf(stderr, "fsr3-golden-vk: cannot write %s\n", path.c_str());
                std::exit(1);
            }
            const auto* halves = static_cast<const uint16_t*>(readback.mapped);
            double sum = 0.0;
            size_t nonfinite = 0;
            for (size_t i = 0; i < size_t(ow) * oh * 4; ++i) {
                const float value = FromHalf(halves[i]);
                if (std::isfinite(value)) {
                    sum += value;
                } else {
                    ++nonfinite;
                }
            }
            std::printf("%s: %ux%u -> %ux%u, mean %.5f, %zu non-finite\n", path.c_str(), w, h, ow,
                        oh, sum / double(size_t(ow) * oh * 4), nonfinite);
        }
    }

    ffxVkPortableUpscaleContextDestroy(context);
    vkFreeCommandBuffers(gpu.device, gpu.pool, 1, &cmd);
    for (const Image* image : {&color, &depth, &motion, &output}) {
        vkDestroyImage(gpu.device, image->image, nullptr);
        vkFreeMemory(gpu.device, image->memory, nullptr);
    }
    for (const Buffer* buffer : {&color_staging, &depth_staging, &motion_staging, &readback}) {
        vkDestroyBuffer(gpu.device, buffer->buffer, nullptr);
        vkFreeMemory(gpu.device, buffer->memory, nullptr);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: fsr3-golden-vk <output directory>\n");
        return 2;
    }
    if (!std::getenv("VK_DRIVER_FILES") && BB_MOLTENVK_ICD[0]) {
        setenv("VK_DRIVER_FILES", BB_MOLTENVK_ICD, 1);
    }
    const Gpu gpu = CreateGpu();
    // ffx_vk picks the fp16 blobs when the device enables shaderFloat16 (wave32 only: no
    // subgroup size control).
    std::printf("blob set: %s, wave32\n", gpu.fp16 ? "fp16 (_16bit)" : "fp32");
    for (const Fsr3Case& c : kCases) {
        RunCase(gpu, c, argv[1]);
    }
    vkDestroyCommandPool(gpu.device, gpu.pool, nullptr);
    vkDestroyDevice(gpu.device, nullptr);
    vkDestroyInstance(gpu.instance, nullptr);
    return 0;
}

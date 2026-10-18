// SPDX-License-Identifier: GPL-2.0-or-later
// Metal back end, M0 (docs/DESIGN_METAL_BACKEND.md): our own Metal encoding between MoltenVK
// command buffers on MoltenVK's queue. Checks, with and without prefilled command buffers
// (MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS, read when the instance is created):
//   - the queue, an image's texture and a memory's buffer export (VK_EXT_metal_objects);
//   - order: Vulkan clear (red) -> our render pass (green, left half) -> Vulkan copy to a buffer;
//     both when the Vulkan copy is recorded before our command buffer is committed and after;
//   - our buffer writes through the exported MTLBuffer reach a Vulkan copy;
//   - the hazard tracking and storage modes MoltenVK gives its resources.
// Build/run: bash build_macos.sh --test (arm64, links MoltenVK directly).
#import <Metal/Metal.h>
#define VK_ENABLE_BETA_EXTENSIONS 1
#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t W = 64, H = 64;
int failures = 0;

#define CHECK_VK(x)                                                                                \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::printf("FAIL: %s = %d\n", #x, r_);                                                \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Vk {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = 0;
    VkCommandPool pool{};
    PFN_vkExportMetalObjectsEXT export_objects{};

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
        VkPhysicalDeviceMemoryProperties p;
        vkGetPhysicalDeviceMemoryProperties(gpu, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want) {
                return i;
            }
        }
        std::printf("FAIL: no memory type\n");
        std::exit(1);
    }

    VkCommandBuffer Begin() const {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cb;
        CHECK_VK(vkAllocateCommandBuffers(device, &ai, &cb));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        CHECK_VK(vkBeginCommandBuffer(cb, &bi));
        return cb;
    }

    void Submit(VkCommandBuffer cb) const {
        CHECK_VK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        CHECK_VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    }
};

Vk CreateVk() {
    Vk vk;
    // MoltenVK linked directly (no loader): no portability enumeration needed.
    const char* inst_ext[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "metal-interop", 1, nullptr, 0,
                          VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = inst_ext;
    CHECK_VK(vkCreateInstance(&ici, nullptr, &vk.instance));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(vk.instance, &n, &vk.gpu);
    Expect(n == 1, "a physical device");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &prio};
    const char* dev_ext[] = {VK_EXT_METAL_OBJECTS_EXTENSION_NAME, "VK_KHR_portability_subset"};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = dev_ext;
    CHECK_VK(vkCreateDevice(vk.gpu, &dci, nullptr, &vk.device));
    vkGetDeviceQueue(vk.device, 0, 0, &vk.queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    CHECK_VK(vkCreateCommandPool(vk.device, &pci, nullptr, &vk.pool));
    vk.export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        vkGetDeviceProcAddr(vk.device, "vkExportMetalObjectsEXT"));
    Expect(vk.export_objects != nullptr, "vkExportMetalObjectsEXT");
    return vk;
}

const char* Tracking(MTLHazardTrackingMode m) {
    return m == MTLHazardTrackingModeTracked ? "tracked"
           : m == MTLHazardTrackingModeUntracked ? "untracked" : "default";
}

const char* Storage(MTLStorageMode m) {
    return m == MTLStorageModeShared ? "shared" : m == MTLStorageModePrivate ? "private"
                                     : m == MTLStorageModeMemoryless ? "memoryless" : "managed";
}

// Our pass: loads the texture and fills the left half with green.
id<MTLRenderPipelineState> GreenPipeline(id<MTLDevice> dev, MTLPixelFormat format) {
    NSString* src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                     "vertex float4 vs(uint id [[vertex_id]]) {\n"
                     "  float2 p[6] = {{-1,-1},{0,-1},{-1,1},{0,-1},{0,1},{-1,1}};\n"
                     "  return float4(p[id], 0, 1); }\n"
                     "fragment float4 fs() { return float4(0, 1, 0, 1); }\n";
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        std::printf("FAIL: MSL: %s\n", err.localizedDescription.UTF8String);
        std::exit(1);
    }
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"fs"];
    d.colorAttachments[0].pixelFormat = format;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) {
        std::printf("FAIL: pipeline: %s\n", err.localizedDescription.UTF8String);
        std::exit(1);
    }
    return ps;
}

void Run(const Vk& vk, bool copy_recorded_first) {
    // Render target image (Vulkan-owned) and a host-visible readback buffer.
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {W, H, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image;
    CHECK_VK(vkCreateImage(vk.device, &ii, nullptr, &image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, image, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
                             vk.MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory image_mem;
    CHECK_VK(vkAllocateMemory(vk.device, &mai, nullptr, &image_mem));
    CHECK_VK(vkBindImageMemory(vk.device, image, image_mem, 0));

    const VkDeviceSize size = W * H * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size * 2; // [0, size): readback; [size, 2*size): written by our Metal blit
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer;
    CHECK_VK(vkCreateBuffer(vk.device, &bci, nullptr, &buffer));
    vkGetBufferMemoryRequirements(vk.device, buffer, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk.MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory buffer_mem;
    CHECK_VK(vkAllocateMemory(vk.device, &mai, nullptr, &buffer_mem));
    CHECK_VK(vkBindBufferMemory(vk.device, buffer, buffer_mem, 0));
    uint8_t* host = nullptr;
    CHECK_VK(vkMapMemory(vk.device, buffer_mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&host)));
    std::memset(host, 0, size * 2);

    // Export the queue, the texture and the buffer.
    VkExportMetalCommandQueueInfoEXT eq{VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT};
    eq.queue = vk.queue;
    VkExportMetalTextureInfoEXT et{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT, &eq};
    et.image = image;
    et.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkExportMetalBufferInfoEXT eb{VK_STRUCTURE_TYPE_EXPORT_METAL_BUFFER_INFO_EXT, &et};
    eb.memory = buffer_mem;
    VkExportMetalObjectsInfoEXT ei{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &eb};
    vk.export_objects(vk.device, &ei);
    id<MTLCommandQueue> queue = eq.mtlCommandQueue;
    id<MTLTexture> texture = et.mtlTexture;
    id<MTLBuffer> mbuffer = eb.mtlBuffer;
    Expect(queue && texture && mbuffer, "exported queue, texture and buffer");
    if (!queue || !texture || !mbuffer) {
        return;
    }
    static bool printed;
    if (!printed) {
        printed = true;
        std::printf("  MoltenVK resources: texture %s/%s, buffer %s/%s, gpuAddress %s\n",
                    Tracking(texture.hazardTrackingMode), Storage(texture.storageMode),
                    Tracking(mbuffer.hazardTrackingMode), Storage(mbuffer.storageMode),
                    mbuffer.gpuAddress ? "yes" : "no");
    }

    // 1. Vulkan: clear the image red.
    VkCommandBuffer clear = vk.Begin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(clear, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    VkClearColorValue red{{1.0f, 0.0f, 0.0f, 1.0f}};
    vkCmdClearColorImage(clear, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &b.subresourceRange);
    vk.Submit(clear);

    // 3. Vulkan: copy the image to the buffer (recorded before or after our commit).
    auto record_copy = [&] {
        VkCommandBuffer copy = vk.Begin();
        VkImageMemoryBarrier c = b;
        c.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        c.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        c.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        c.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(copy, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &c);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(copy, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        return copy;
    };
    VkCommandBuffer copy = copy_recorded_first ? record_copy() : VK_NULL_HANDLE;

    // 2. Ours: a render pass that loads the image and draws green over the left half, then a blit
    // that writes a pattern into the second half of the Vulkan buffer.
    static id<MTLRenderPipelineState> green;
    if (!green) {
        green = GreenPipeline(queue.device, texture.pixelFormat);
    }
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    cmd.label = @"bbport Metal interop";
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:green];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit fillBuffer:mbuffer range:NSMakeRange(size, size) value:0x5a];
    [blit endEncoding];
    [cmd commit];

    if (!copy) {
        copy = record_copy();
    }
    vk.Submit(copy);
    CHECK_VK(vkQueueWaitIdle(vk.queue));
    [cmd waitUntilCompleted];

    auto px = [&](uint32_t x, uint32_t y) {
        const uint8_t* p = host + (y * W + x) * 4;
        return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    };
    const uint32_t left = px(8, 32), right = px(56, 32);
    const bool order_ok = left == 0x00ff00 && right == 0xff0000;
    std::printf("  copy recorded %s our commit: left %06x right %06x -> %s\n",
                copy_recorded_first ? "before" : "after", left, right,
                order_ok ? "ordered" : "NOT ordered");
    Expect(order_ok, "Vulkan clear -> our pass -> Vulkan copy, in queue order");
    Expect(host[size] == 0x5a && host[2 * size - 1] == 0x5a, "our blit into the Vulkan buffer");

    vkDestroyBuffer(vk.device, buffer, nullptr);
    vkFreeMemory(vk.device, buffer_mem, nullptr);
    vkDestroyImage(vk.device, image, nullptr);
    vkFreeMemory(vk.device, image_mem, nullptr);
}

} // namespace

int main() {
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 0); // errors only
    @autoreleasepool {
        const char* prefill = std::getenv("MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS");
        std::printf("Metal interop (prefill %s):\n", prefill ? prefill : "off");
        Vk vk = CreateVk();
        Run(vk, false);
        Run(vk, true);
        vkDestroyCommandPool(vk.device, vk.pool, nullptr);
        vkDestroyDevice(vk.device, nullptr);
        vkDestroyInstance(vk.instance, nullptr);
    }
    if (failures) {
        std::printf("FAIL: Metal interop, %d check(s)\n", failures);
        return 1;
    }
    std::puts("PASS: Metal interop: MoltenVK queue/texture/buffer export, our encoders in queue order");
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MTLBuffer creation for the buffer cache (Metal-native renderer).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>

#include "video_core/renderer_metal/mtl_buffer.h"
#include "video_core/renderer_metal/mtl_device.h"

namespace Metal {

namespace {

MTLResourceOptions OptionsFor(const Device& device, VideoCore::MemoryType type) {
    switch (type) {
    case VideoCore::MemoryType::DeviceLocal:
        // Untracked: the Encoder orders them (mtl_order.h); shared buffers stay tracked.
        return MTLResourceStorageModePrivate |
               (device.HazardTracking() ? 0 : MTLResourceHazardTrackingModeUntracked);
    case VideoCore::MemoryType::HostUncached:
        return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
    case VideoCore::MemoryType::HostCached:
    case VideoCore::MemoryType::Stream:
    default:
        return MTLResourceStorageModeShared;
    }
}

/// BB_ARENA_LAZY (default 1, 0 = off): device-local buffers of 1 GiB and more (the guest arena)
/// live in vm_allocate'd memory wrapped without copying, so only the pages the GPU or an upload
/// touches cost memory. A private buffer is committed in full when created: the 16 GiB arena made
/// the GPU process ~24 GB (measured 2026-10-17: 4/8/16 GiB arena -> 11/15/24 GB).
bool LazyArena(VideoCore::MemoryType type, u64 size) {
    static const bool on = [] {
        const char* env = std::getenv("BB_ARENA_LAZY");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return on && type == VideoCore::MemoryType::DeviceLocal && size >= (u64{1} << 30);
}

id<MTLBuffer> NewLazyBuffer(id<MTLDevice> mtl_device, const Device& device, u64 size) {
    vm_address_t address = 0;
    const vm_size_t length = round_page(size);
    if (vm_allocate(mach_task_self(), &address, length, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        return nil;
    }
    const MTLResourceOptions options =
        MTLResourceStorageModeShared |
        (device.HazardTracking() ? 0 : MTLResourceHazardTrackingModeUntracked);
    id<MTLBuffer> buffer = [mtl_device newBufferWithBytesNoCopy:reinterpret_cast<void*>(address)
                                                         length:length
                                                        options:options
                                                    deallocator:^(void* pointer, NSUInteger bytes) {
                                                      vm_deallocate(mach_task_self(),
                                                                    vm_address_t(pointer), bytes);
                                                    }];
    if (buffer == nil) {
        vm_deallocate(mach_task_self(), address, length);
    }
    return buffer;
}

} // namespace

Object CreateBuffer(Device& device, u64 size, VideoCore::MemoryType type, std::string_view label,
                    u64* gpu_address, u8** contents) {
    *gpu_address = 0;
    *contents = nullptr;
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLBuffer> buffer = LazyArena(type, size)
                               ? NewLazyBuffer(mtl_device, device, size)
                               : [mtl_device newBufferWithLength:size options:OptionsFor(device, type)];
    if (buffer == nil) {
        return {};
    }
    if (!label.empty()) {
        buffer.label = [[NSString alloc] initWithBytes:label.data()
                                                length:label.size()
                                              encoding:NSUTF8StringEncoding];
    }
    *gpu_address = buffer.gpuAddress;
    if (type != VideoCore::MemoryType::DeviceLocal) {
        *contents = static_cast<u8*>(buffer.contents);
    }
    return Object::Adopt((__bridge_retained void*)buffer);
}

} // namespace Metal

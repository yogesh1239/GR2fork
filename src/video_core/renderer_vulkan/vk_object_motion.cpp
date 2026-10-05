// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <utility>

#include <vk_mem_alloc.h>

namespace Vulkan {

static constexpr vk::Format MotionFormat = vk::Format::eR32G32B32A32Sfloat;
static constexpr vk::ImageSubresourceRange MotionRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

ObjectMotion::ObjectMotion(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_},
      image{instance_.GetDevice(), instance_.GetAllocator()} {}

void ObjectMotion::Enable() {
    if (std::exchange(tried, true) || !instance.IsFsr411Fp8Supported()) {
        return;
    }
    const auto features =
        instance.GetPhysicalDevice()
            .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
    if (!features.get().features.vertexPipelineStoresAndAtomics ||
        !features.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress ||
        !instance.IsUnusedAttachmentsSupported() ||
        !instance.IsFormatSupported(MotionFormat,
                                    vk::FormatFeatureFlagBits2::eColorAttachment |
                                        vk::FormatFeatureFlagBits2::eColorAttachmentBlend |
                                        vk::FormatFeatureFlagBits2::eSampledImage)) {
        LOG_WARNING(Render_Vulkan, "FSR 4.1.1 object motion: unsupported by the device, off");
        return;
    }
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(1 + 2 * PositionsPerFrame) * 16,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buffer{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer,
                        &positions_allocation, nullptr) != VK_SUCCESS) {
        LOG_WARNING(Render_Vulkan, "FSR 4.1.1 object motion: no position buffer, off");
        return;
    }
    positions_buffer = buffer;
    positions_address = instance.GetDevice().getBufferAddress({.buffer = positions_buffer});
    LOG_INFO(Render_Vulkan, "FSR 4.1.1 object motion: on, {} vertices per frame",
             PositionsPerFrame);
}

ObjectMotion::~ObjectMotion() {
    if (positions_buffer) {
        scheduler.Finish();
        vmaDestroyBuffer(instance.GetAllocator(), positions_buffer, positions_allocation);
    }
}

Shader::PushData::Motion ObjectMotion::PrepareDraw(const Motion::Draw& draw) {
    const auto a = history.Prepare(draw);
    if (!a.store && !a.load) {
        return {};
    }
    return {a.store, a.load, a.vertices, a.first_vertex, a.first_instance, a.instances};
}

bool ObjectMotion::PrepareRead(u32 width, u32 height) {
    if (!image || width != image_width || height != image_height) {
        return false;
    }
    scheduler.EndRendering();
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = MotionRange,
    };
    scheduler.CommandBuffer().pipelineBarrier2(
        {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    return true;
}

bool ObjectMotion::EndFrame(u32 width, u32 height, bool gap) {
    // Also when all are 0: a selection that never matches shows here.
    if (frame % 600 == 599) {
        const auto& s = history.stats;
        const auto& r = index_ranges.stats;
        LOG_INFO(Render_Vulkan,
                 "FSR 4.1.1 object motion (600 frames): {} motion-pipeline draws ({} blended), {} "
                 "on the scene, {} stored, {} with history, {} unmatched, {} "
                 "over capacity, {} invalid; last frame {} of {} vertices; index ranges {} "
                 "reused, {} scanned, {} changed",
                 draws, blended, s.draws, s.stored, s.loaded, s.unmatched, s.exhausted, s.invalid,
                 history.Used(), PositionsPerFrame, r.hits, r.scans, r.stale);
        history.stats = {};
        index_ranges.stats = {};
        draws = blended = 0;
    }
    history.NextFrame(gap);
    if (++frame % Motion::IndexRangeCache::Unused == 0) {
        index_ranges.Trim(frame);
    }

    const bool create = !image || width != image_width || height != image_height;
    if (create) {
        if (image) {
            scheduler.Finish();
            view.reset();
            image.Destroy();
        }
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = MotionFormat,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferDst,
        });
        view = Check<"object motion view">(instance.GetDevice().createImageViewUnique({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = MotionFormat,
            .subresourceRange = MotionRange,
        }));
        image_width = width;
        image_height = height;
    }

    // Validity 0 everywhere; the draws of the next frame write over it.
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageMemoryBarrier2 to_clear{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                        vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = create ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = MotionRange,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear});
    cmdbuf.clearColorImage(image, vk::ImageLayout::eGeneral, vk::ClearColorValue{}, MotionRange);
    const vk::ImageMemoryBarrier2 to_draw{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask =
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = MotionRange,
    };
    // This frame's stores are read by the next frame's loads, and the half read now is written
    // again the frame after.
    const vk::MemoryBarrier2 positions{
        .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .srcAccessMask =
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                             .pMemoryBarriers = &positions,
                             .imageMemoryBarrierCount = 1,
                             .pImageMemoryBarriers = &to_draw});
    return create;
}

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <utility>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "video_core/host_shaders/fsr411_motion_comp.h"
#include "video_core/host_shaders/fsr411_motion_view_comp.h"
#include "video_core/host_shaders/fsr411_store_comp.h"
#include "video_core/renderer_vulkan/vk_fsr411_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/skipcache/skipcache.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

Fsr411Pass::Fsr411Pass(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_},
      upscaled{instance_.GetDevice(), instance_.GetAllocator()},
      merged{instance_.GetDevice(), instance_.GetAllocator()},
      cover{VideoCore::UniqueImage{instance_.GetDevice(), instance_.GetAllocator()},
            VideoCore::UniqueImage{instance_.GetDevice(), instance_.GetAllocator()}} {}

Fsr411Pass::~Fsr411Pass() {
    if (!ticks.empty()) {
        scheduler.Wait(ticks.back());
    }
}

bool Fsr411Pass::Record(Fsr411::Frame& frame) {
    if (failed) {
        return false;
    }
    if (!instance.IsFsr411Fp8Supported()) {
        LOG_ERROR(Render_Vulkan, "FSR 4.1.1: the GPU lacks the FP8 features, it stays off");
        failed = true;
        return false;
    }
    if (!upscaler) {
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "fsr4_411";
        upscaler = std::make_unique<Fsr411::Upscaler>(
            instance.GetPhysicalDevice(), instance.GetDevice(), Common::FS::PathToUTF8String(dir));
    }
    while (ticks.size() >= Fsr411::kFramesInFlight) {
        scheduler.Wait(ticks.front());
        ticks.pop_front();
    }
    scheduler.EndRendering();
    frame.cmdbuf = scheduler.CommandBuffer();
    // Its pipelines and push descriptors replace the compute state the dedup caches remember,
    // also when it fails after recording some passes.
    auto& skipcache = VideoCore::Skipcache::Framework::Instance();
    skipcache.BumpForeignPipelineGen(1);
    skipcache.BumpForeignPushGen(1);
    if (!upscaler->Record(frame)) {
        const std::string& error = upscaler->Error();
        if (error != logged_error) {
            LOG_ERROR(Render_Vulkan, "FSR 4.1.1: {}", error);
            logged_error = error;
        }
        failed = !error.starts_with("unsupported size");
        return false;
    }
    ticks.push_back(scheduler.CurrentTick());
    if (std::string d = upscaler->Describe(); d != described) {
        described = std::move(d);
        LOG_INFO(Render_Vulkan, "FSR 4.1.1: {}", described);
    }
    return true;
}

bool Fsr411Pass::SelfTest(u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    struct Scratch {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
    };
    std::array<Scratch, 4> scratch;
    const auto make = [&](Scratch& s, vk::Format format, vk::ImageUsageFlags usage,
                          vk::ImageAspectFlags aspect) {
        s.image = VideoCore::UniqueImage{device, instance.GetAllocator()};
        s.image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage | vk::ImageUsageFlagBits::eSampled,
        });
        auto [result, view] = device.createImageViewUnique(vk::ImageViewCreateInfo{
            .image = s.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        });
        ASSERT(result == vk::Result::eSuccess);
        s.view = std::move(view);
    };
    constexpr auto color = vk::ImageAspectFlagBits::eColor;
    make(scratch[0], vk::Format::eR16G16B16A16Sfloat, vk::ImageUsageFlagBits::eStorage, color);
    make(scratch[1], vk::Format::eD32Sfloat, vk::ImageUsageFlagBits::eDepthStencilAttachment,
         vk::ImageAspectFlagBits::eDepth);
    make(scratch[2], vk::Format::eR16G16Sfloat, vk::ImageUsageFlagBits::eStorage, color);
    make(scratch[3], vk::Format::eR16G16B16A16Sfloat, vk::ImageUsageFlagBits::eStorage, color);

    scheduler.EndRendering();
    std::array<vk::ImageMemoryBarrier2, 4> barriers;
    for (u32 i = 0; i < barriers.size(); ++i) {
        barriers[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = scratch[i].image,
            .subresourceRange = {i == 1 ? vk::ImageAspectFlagBits::eDepth : color, 0, 1, 0, 1},
        };
    }
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
    const auto image = [&](const Scratch& s) {
        return Fsr411::Image{static_cast<VkImage>(s.image.image), static_cast<VkImageView>(*s.view),
                             width, height};
    };
    Fsr411::Frame frame{
        .color = image(scratch[0]),
        .depth = image(scratch[1]),
        .motion = image(scratch[2]),
        .output = image(scratch[3]),
        .render_width = width,
        .render_height = height,
        .reset = true,
    };
    const bool recorded = Record(frame);
    scheduler.Finish();
    LOG_INFO(Render_Vulkan, "FSR 4.1.1 self-test at {}x{}: {}", width, height,
             recorded ? "passed" : "failed");
    return recorded;
}

bool Fsr411Pass::RecordAa(Fsr411::Frame& frame, vk::ImageView output, vk::ImageView object_motion,
                          AaView view, bool mark_uncovered) {
    if (failed) {
        return false;
    }
    const vk::Device device = instance.GetDevice();
    const u32 width = frame.color.width, height = frame.color.height;
    const bool create = !upscaled || upscaled.image_ci.extent.width != width ||
                        upscaled.image_ci.extent.height != height;
    if (create) {
        if (upscaled) {
            // The runtime also frees its size-bound resources on a size change, without a wait.
            scheduler.Finish();
            ticks.clear();
            upscaled_view.reset();
            upscaled.Destroy();
            merged_view.reset();
            merged.Destroy();
            for (u32 i = 0; i < cover.size(); ++i) {
                cover_views[i].reset();
                cover[i].Destroy();
            }
        }
        const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                              vk::Format format) {
            image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {width, height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            });
            view = Check<"fsr411 image view">(device.createImageViewUnique({
                .image = image,
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        };
        make(upscaled, upscaled_view, vk::Format::eR16G16B16A16Sfloat);
        make(merged, merged_view, vk::Format::eR16G16Sfloat);
        // Unwritten until the first merge: FSR resets its history on this frame, which makes
        // the marks of that merge void.
        for (u32 i = 0; i < cover.size(); ++i) {
            make(cover[i], cover_views[i], vk::Format::eR8Unorm);
        }
        frame.reset = true;
    }
    if (!store.pipeline) {
        sampler = Check<"fsr411 sampler">(device.createSamplerUnique({
            .magFilter = vk::Filter::eNearest,
            .minFilter = vk::Filter::eNearest,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
            .maxAnisotropy = 1.0f,
        }));
        store = CreatePass(FSR411_STORE_COMP, 2, 1);
        merge = CreatePass(FSR411_MOTION_COMP, 4, 2, true);
        motion_view = CreatePass(FSR411_MOTION_VIEW_COMP, 4, 1);
        if (const char* profile = std::getenv("BB_FSR4_PROFILE"); profile && profile[0] == '1') {
            merge_queries = Check<"fsr411 merge queries">(device.createQueryPoolUnique({
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = 2 * MergeSlots,
            }));
            merge_marks.fill(-1);
            timestamp_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        }
    }

    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    // The last store pass read `upscaled`, the last FSR run `merged` and the last merge `cover`;
    // all are written again.
    const vk::MemoryBarrier2 reuse{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    const std::array new_images{upscaled.image, merged.image, cover[0].image, cover[1].image};
    std::array<vk::ImageMemoryBarrier2, new_images.size()> init;
    for (u32 i = 0; i < init.size(); ++i) {
        init[i] = {
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = new_images[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &reuse,
        .imageMemoryBarrierCount = create ? static_cast<u32>(init.size()) : 0u,
        .pImageMemoryBarriers = init.data(),
    });

    if (object_motion) {
        // As in Record: this compute state replaces what the dedup caches remember.
        auto& skipcache = VideoCore::Skipcache::Framework::Instance();
        skipcache.BumpForeignPipelineGen(1);
        skipcache.BumpForeignPushGen(1);
        // Last frame's cover in, this frame's out.
        const u32 last = cover_index;
        cover_index ^= 1;
        const std::array<vk::DescriptorImageInfo, 6> infos{{
            {.imageView = frame.motion.view, .imageLayout = vk::ImageLayout(frame.motion.layout)},
            {.imageView = frame.depth.view, .imageLayout = vk::ImageLayout(frame.depth.layout)},
            {.imageView = object_motion, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *cover_views[last], .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *merged_view, .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *cover_views[cover_index], .imageLayout = vk::ImageLayout::eGeneral},
        }};
        if (merge_queries) {
            CollectMergeTime();
            cmdbuf.resetQueryPool(*merge_queries, 2 * merge_slot, 2);
            cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eComputeShader, *merge_queries,
                                   2 * merge_slot);
        }
        Dispatch(cmdbuf, merge, infos, 4, width, height, mark_uncovered ? 1u : 0u);
        if (merge_queries) {
            cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eComputeShader, *merge_queries,
                                   2 * merge_slot + 1);
            merge_marks[merge_slot] = mark_uncovered ? 1 : 0;
            merge_slot = (merge_slot + 1) % MergeSlots;
        }
        const vk::MemoryBarrier2 written{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
        frame.motion = {static_cast<VkImage>(merged.image), static_cast<VkImageView>(*merged_view),
                        width, height};
        if (view == AaView::Motion) {
            const std::array<vk::DescriptorImageInfo, 5> shown{{
                {.imageView = frame.color.view, .imageLayout = vk::ImageLayout(frame.color.layout)},
                infos[1],
                infos[2],
                {.imageView = *merged_view, .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = output, .imageLayout = vk::ImageLayout::eGeneral},
            }};
            Dispatch(cmdbuf, motion_view, shown, 4, width, height);
            skipped = true;
            return true;
        }
    }
    if (view == AaView::Input) {
        // The store pass encodes the sRGB-decoded colour again: the game's bytes, unchanged.
        const std::array<vk::DescriptorImageInfo, 3> shown{{
            {.imageView = frame.color.view, .imageLayout = vk::ImageLayout(frame.color.layout)},
            {.imageView = frame.color.view, .imageLayout = vk::ImageLayout(frame.color.layout)},
            {.imageView = output, .imageLayout = vk::ImageLayout::eGeneral},
        }};
        Dispatch(cmdbuf, store, shown, 2, width, height);
        skipped = true;
        return true;
    }
    frame.reset |= std::exchange(skipped, false);

    frame.output = {static_cast<VkImage>(upscaled.image), static_cast<VkImageView>(*upscaled_view),
                    width, height};
    frame.render_width = width;
    frame.render_height = height;
    if (!Record(frame)) {
        return false;
    }

    // Record ends with a compute write-to-read barrier, which covers this read of `upscaled`.
    const std::array<vk::DescriptorImageInfo, 3> infos{{
        {.imageView = *upscaled_view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = frame.color.view, .imageLayout = vk::ImageLayout(frame.color.layout)},
        {.imageView = output, .imageLayout = vk::ImageLayout::eGeneral},
    }};
    Dispatch(cmdbuf, store, infos, 2, width, height);
    return true;
}

Fsr411Pass::Pass Fsr411Pass::CreatePass(std::span<const u32> code, u32 samplers, u32 storages,
                                        bool push) {
    const vk::Device device = instance.GetDevice();
    const vk::Sampler immutable = *sampler;
    std::array<vk::DescriptorSetLayoutBinding, 6> bindings{};
    const u32 count = samplers + storages;
    ASSERT(count <= bindings.size());
    for (u32 i = 0; i < count; ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = i < samplers ? vk::DescriptorType::eCombinedImageSampler
                                           : vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = i < samplers ? &immutable : nullptr,
        };
    }
    Pass pass;
    pass.set_layout = Check<"fsr411 set layout">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
        .bindingCount = count,
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(u32)};
    pass.layout = Check<"fsr411 pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*pass.set_layout,
        .pushConstantRangeCount = push ? 1u : 0u,
        .pPushConstantRanges = &push_range,
    }));
    pass.push = push;
    const vk::ShaderModule module = CompileSPV(code, device);
    ASSERT(module);
    const vk::ComputePipelineCreateInfo pipeline_ci{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
        },
        .layout = *pass.layout,
    };
    pass.pipeline = Check<"fsr411 pipeline">(device.createComputePipelineUnique({}, pipeline_ci));
    device.destroyShaderModule(module);
    return pass;
}

void Fsr411Pass::CollectMergeTime() {
    auto& marks = merge_marks[merge_slot];
    std::array<u64, 2> ts{};
    if (marks < 0 || instance.GetDevice().getQueryPoolResults(
                         *merge_queries, 2 * merge_slot, 2, sizeof(ts), ts.data(), sizeof(u64),
                         vk::QueryResultFlagBits::e64) != vk::Result::eSuccess) {
        return; // empty, or not done yet: this sample is lost
    }
    merge_ms[marks] += double(ts[1] - ts[0]) * timestamp_ns * 1e-6;
    ++merge_frames[marks];
    marks = -1;
    if ((merge_frames[0] + merge_frames[1]) % 300 == 0) {
        const auto average = [&](u32 i) {
            return merge_frames[i] ? merge_ms[i] / merge_frames[i] : 0.0;
        };
        LOG_INFO(Render_Vulkan,
                 "FSR 4.1.1 merge pass (GPU): {:.4f} ms per frame with the marks ({} frames), "
                 "{:.4f} ms without ({} frames)",
                 average(1), merge_frames[1], average(0), merge_frames[0]);
        merge_ms = {};
        merge_frames = {};
    }
}

void Fsr411Pass::Dispatch(vk::CommandBuffer cmdbuf, const Pass& pass,
                          std::span<const vk::DescriptorImageInfo> images, u32 samplers, u32 width,
                          u32 height, u32 push) {
    std::array<vk::WriteDescriptorSet, 6> writes{};
    ASSERT(images.size() <= writes.size());
    for (u32 i = 0; i < images.size(); ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = i < samplers ? vk::DescriptorType::eCombinedImageSampler
                                           : vk::DescriptorType::eStorageImage,
            .pImageInfo = &images[i],
        };
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pass.pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pass.layout, 0,
                                std::span{writes.data(), images.size()});
    if (pass.push) {
        cmdbuf.pushConstants(*pass.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
    }
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

} // namespace Vulkan

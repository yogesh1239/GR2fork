// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/logging/log.h"
#include "common/path_util.h"
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
      upscaled{instance_.GetDevice(), instance_.GetAllocator()} {}

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

bool Fsr411Pass::RecordAa(Fsr411::Frame& frame, vk::ImageView output) {
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
        }
        upscaled.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        });
        upscaled_view = Check<"fsr411 upscaled view">(device.createImageViewUnique({
            .image = upscaled,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        frame.reset = true;
    }
    if (!store_pipeline) {
        CreateStorePass();
    }

    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    // The last store pass read `upscaled`, and FSR writes it again.
    const vk::MemoryBarrier2 reuse{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    const vk::ImageMemoryBarrier2 init{
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = upscaled,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &reuse,
        .imageMemoryBarrierCount = create ? 1u : 0u,
        .pImageMemoryBarriers = &init,
    });

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
    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &infos[i],
        };
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *store_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *store_layout, 0, writes);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

void Fsr411Pass::CreateStorePass() {
    const vk::Device device = instance.GetDevice();
    store_sampler = Check<"fsr411 store sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
    }));
    const vk::Sampler sampler = *store_sampler;
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = i < 2 ? &sampler : nullptr,
        };
    }
    store_set_layout = Check<"fsr411 store set layout">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    store_layout = Check<"fsr411 store pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*store_set_layout,
    }));
    const vk::ShaderModule module = CompileSPV(FSR411_STORE_COMP, device);
    ASSERT(module);
    const vk::ComputePipelineCreateInfo pipeline_ci{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
        },
        .layout = *store_layout,
    };
    store_pipeline =
        Check<"fsr411 store pipeline">(device.createComputePipelineUnique({}, pipeline_ci));
    device.destroyShaderModule(module);
}

} // namespace Vulkan

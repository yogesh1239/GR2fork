// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "video_core/renderer_vulkan/vk_fsr411_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

Fsr411Pass::Fsr411Pass(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

Fsr411Pass::~Fsr411Pass() = default;

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
    if (!upscaler->Record(frame)) {
        const std::string& error = upscaler->Error();
        LOG_ERROR(Render_Vulkan, "FSR 4.1.1: {}", error);
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

} // namespace Vulkan

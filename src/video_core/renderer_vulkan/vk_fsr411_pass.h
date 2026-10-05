// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <memory>
#include <string>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

#include "video_core/renderer_vulkan/fsr411/fsr411.h"

namespace Vulkan {

class Instance;
class Scheduler;

class Fsr411Pass {
public:
    explicit Fsr411Pass(const Instance& instance, Scheduler& scheduler);
    ~Fsr411Pass();

    [[nodiscard]] bool IsAvailable() const {
        return !failed;
    }

    /// Records into the scheduler's command buffer; the images must be in layout General.
    bool Record(Fsr411::Frame& frame);

    /// Builds the passes and runs one frame on scratch images, then waits for the GPU.
    bool SelfTest(u32 width, u32 height);

    /// In place of the game's AA: FSR at the size of frame.color into an RGBA16F image, then a
    /// store pass writes it sRGB-encoded, with the alpha of frame.color, into `output` (a
    /// storage view in General). Sets frame.output; a size change sets frame.reset.
    bool RecordAa(Fsr411::Frame& frame, vk::ImageView output);

private:
    void CreateStorePass();

    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<Fsr411::Upscaler> upscaler;
    std::deque<u64> ticks;
    std::string described;
    std::string logged_error;
    bool failed{};

    VideoCore::UniqueImage upscaled;
    vk::UniqueImageView upscaled_view;
    vk::UniqueSampler store_sampler;
    vk::UniqueDescriptorSetLayout store_set_layout;
    vk::UniquePipelineLayout store_layout;
    vk::UniquePipeline store_pipeline;
};

} // namespace Vulkan

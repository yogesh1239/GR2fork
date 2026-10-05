// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deque>
#include <memory>
#include <span>
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

    /// What RecordAa writes to the output: FSR's result, or for a test the game's image as it
    /// is or the characters' vectors over it (fsr411_motion_view.comp; needs `object_motion`).
    enum class AaView { Fsr, Input, Motion };

    /// In place of the game's AA: FSR at the size of frame.color into an RGBA16F image, then a
    /// store pass writes it sRGB-encoded, with the alpha of frame.color, into `output` (a
    /// storage view in General). Sets frame.output; a size change sets frame.reset. With
    /// `object_motion` (ObjectMotion's image, General, read barrier recorded), a merge pass first
    /// lays its vectors over frame.motion and, with `mark_uncovered`, takes the history from the
    /// background the characters uncover (fsr411_motion.comp); FSR reads the result. FSR's first
    /// frame after a test view resets its history.
    bool RecordAa(Fsr411::Frame& frame, vk::ImageView output, vk::ImageView object_motion = {},
                  AaView view = AaView::Fsr, bool mark_uncovered = true);

private:
    /// A compute pass with push descriptors: `samplers` combined image samplers (nearest),
    /// then `storages` storage images, and with `push` one u32 push constant; one dispatch per
    /// 8x8 pixels.
    struct Pass {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
        bool push{};
    };
    Pass CreatePass(std::span<const u32> code, u32 samplers, u32 storages, bool push = false);
    void Dispatch(vk::CommandBuffer cmdbuf, const Pass& pass,
                  std::span<const vk::DescriptorImageInfo> images, u32 samplers, u32 width,
                  u32 height, u32 push = 0);

    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<Fsr411::Upscaler> upscaler;
    std::deque<u64> ticks;
    std::string described;
    std::string logged_error;
    bool failed{};

    VideoCore::UniqueImage upscaled;
    vk::UniqueImageView upscaled_view;
    VideoCore::UniqueImage merged; ///< the motion FSR reads with object motion (RG16F)
    vk::UniqueImageView merged_view;
    /// The characters' pixels, grown by 1, of the last merge and of this one (R8).
    std::array<VideoCore::UniqueImage, 2> cover;
    std::array<vk::UniqueImageView, 2> cover_views;
    u32 cover_index{};
    vk::UniqueSampler sampler;
    Pass store;
    Pass merge;
    Pass motion_view;
    bool skipped{}; ///< the last frame showed a test view, so FSR's history is stale

    /// BB_FSR4_PROFILE=1 (as the runtime's per-pass times): GPU time of the merge pass, with and
    /// without the marks, read when a slot comes round again, logged every 300 merges.
    void CollectMergeTime();
    static constexpr u32 MergeSlots = 8;
    vk::UniqueQueryPool merge_queries;
    std::array<s8, MergeSlots> merge_marks{}; ///< per slot: -1 empty, else the marks flag
    u32 merge_slot{};
    std::array<double, 2> merge_ms{};
    std::array<u32, 2> merge_frames{};
    float timestamp_ns{};
};

} // namespace Vulkan

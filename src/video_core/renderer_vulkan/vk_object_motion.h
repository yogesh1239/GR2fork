// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// FSR 4.1.1 object motion, after bbport's. GR2 draws its characters forward-shaded without
/// velocity. Their pipelines (PipelineCache, MotionDraw) store each vertex's clip position per
/// draw and frame; a draw matched to last frame's (motion_history.h) loads the old position too,
/// and its fragments write the difference into an image at attachment 7, which the merge pass
/// lays over the game's velocity image.
class ObjectMotion {
public:
    ObjectMotion(const Instance& instance, Scheduler& scheduler);
    ~ObjectMotion();

    /// Allocates the position history on the first call (the first FSR frame of GR2), so that
    /// other games and FSR-off sessions never do; Enabled() tells whether it worked.
    void Enable();

    [[nodiscard]] bool Enabled() const {
        return positions_address != 0;
    }
    /// Buffer device address of the clip positions, for PushData::motion_positions.
    [[nodiscard]] u64 Positions() const {
        return positions_address;
    }

    /// The draw's history slots for PushData::motion; all zero when it gets none.
    Shader::PushData::Motion PrepareDraw(const Motion::Draw& draw);
    /// Index range and topology hash of an indexed draw, reused across frames.
    template <class Scan>
    Motion::IndexRangeCache::Result IndexRange(const Motion::IndexRangeCache::Key& key,
                                               Scan&& scan) {
        return index_ranges.Get(key, frame, scan);
    }

    /// The motion image (layout General) when it covers width x height, else null.
    [[nodiscard]] vk::ImageView View(u32 width, u32 height) const {
        return image && width <= image_width && height <= image_height ? *view : vk::ImageView{};
    }
    /// Whether the image holds this frame's vectors for a width x height scene: it was cleared
    /// at that size when the last frame ended. If so, records the barrier for a compute read.
    bool PrepareRead(u32 width, u32 height);

    /// At the end of a frame FSR ran on, after the merge pass read the image: sizes the image to
    /// the scene, clears it and moves the history on (`gap`: forgets it). True when the image
    /// was (re)created.
    bool EndFrame(u32 width, u32 height, bool gap);

    u64 draws{};   ///< motion-pipeline draws, for the statistics
    u64 blended{}; ///< of those, blended or without depth writes: not tracked

private:
    const Instance& instance;
    Scheduler& scheduler;

    // Positions (device local): two halves (this frame, last frame) of vec4; element 0 is unused.
    static constexpr u32 PositionsPerFrame = 4u << 20;
    Motion::History history{PositionsPerFrame};
    Motion::IndexRangeCache index_ranges;
    vk::Buffer positions_buffer{};
    VmaAllocation positions_allocation{};
    u64 positions_address{};
    bool tried{};

    u64 frame{};
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    u32 image_width{}, image_height{};
};

} // namespace Vulkan

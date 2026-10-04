// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <memory>
#include <string>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

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

private:
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<Fsr411::Upscaler> upscaler;
    std::deque<u64> ticks;
    std::string described;
    bool failed{};
};

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include "common/assert.h"
#include "common/cpu_pause.h"
#include "common/debug.h"
#include "common/rdtsc.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/buffer_cache/stream_copy_lane.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/skipcache/skipcache.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    pop_poll_throttle_ = std::max<u32>(EmulatorSettings.GetPendingPopThrottle(), 1u);
    BeginSession();
    if (threaded_recording) {
        recorder_kick_bytes = size_t{std::max(EmulatorSettings.GetVkRecordKickKb(), 1u)} * 1024;
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        // Self-check before any game command: chunk overflow, hand-over, execution, recycling.
        u32 ran = 0;
        for (u32 i = 0; i < 10000; ++i) {
            Record([&ran](vk::CommandBuffer) { ++ran; });
        }
        SyncRecording();
        ASSERT_MSG(ran == 10000, "Recording thread self-check: {} of 10000 commands ran", ran);
    }
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    ++rs_calls_;
#ifndef NDEBUG
    // While the colour tail is still zeroed by every producer, the narrowed
    // relation and the whole-struct one must agree exactly.
    DEBUG_ASSERT((render_state == new_state) ==
                 (std::memcmp(&render_state, &new_state, sizeof(RenderState)) == 0));
#endif
    rs_interrupted_ += !is_rendering;
    if (is_rendering && render_state == new_state) {
        return;
    }
    ++rs_restarts_;
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    Record([color_attachments, depth_attachment, stencil_attachment,
            extent = vk::Extent2D{render_state.width, render_state.height},
            num_layers = render_state.num_layers,
            num_color_attachments = render_state.num_color_attachments, has_depth = db.has_depth,
            has_stencil = db.has_stencil](vk::CommandBuffer cmdbuf) {
        const vk::RenderingInfo rendering_info = {
            .renderArea =
                {
                    .offset = {0, 0},
                    .extent = extent,
                },
            .layerCount = num_layers,
            .colorAttachmentCount = num_color_attachments,
            .pColorAttachments = color_attachments.data(),
            .pDepthAttachment = has_depth ? &depth_attachment : nullptr,
            .pStencilAttachment = has_stencil ? &stencil_attachment : nullptr,
        };
        cmdbuf.beginRendering(rendering_info);
    });
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    upload_cmdbuf = command_pool.Commit();
    Check(upload_cmdbuf.begin(begin_info));
    return upload_cmdbuf;
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    const u64 t0 = Common::FencedRDTSC();
    Wait(presubmit_tick);
    RecordWait(WaitSite::Finish, Common::FencedRDTSC() - t0);
}

u64 Scheduler::WaitTagged(u64 tick, WaitSite site) {
    if (work_semaphore.IsFree(tick)) {
        Wait(tick); // still needs the flush path; it will not block
        return 0;
    }
    const u64 t0 = Common::FencedRDTSC();
    Wait(tick);
    const u64 blocked = Common::FencedRDTSC() - t0;
    RecordWait(site, blocked);
    return blocked;
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    work_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    // Callers poll at draw rate and the queue is empty in steady state; the
    // count gate spares those polls the lock and the semaphore query ioctl.
    // Waiters needing a fresher tick refresh on their own miss paths.
    if (pending_ops_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    // While an op waits out its GPU tick the queue stays non-empty and every
    // draw would otherwise take the lock and issue a SYNCOBJ_QUERY ioctl;
    // the ops are latency-tolerant (deferred frees - urgent writebacks ride
    // the priority queue), so attempt the pop once per N non-empty polls
    // instead. An op retires at most N draws late. A 0 setting latches as 1.
    if (++pop_poll_counter_ < pop_poll_throttle_) {
        return;
    }
    pop_poll_counter_ = 0;
    std::unique_lock lk(pending_ops_mutex);
    work_semaphore.Refresh();
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        pending_ops_count.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::BeginSession() {
    EndSession();

    auto& session = sessions.emplace_back();

    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    session.primary = command_pool.Commit();
    Check(session.primary.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::EndSession() {
    if (sessions.empty()) {
        return;
    }

    if (on_session) {
        on_session();
    }

    const auto& session = sessions.back();
    if (session.upload) {
        Check(session.upload.end());
    }

    EndRendering();
    SyncRecording();
    Check(session.primary.end());
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    std::scoped_lock lk{submit_mutex};
    // Every submit of this scheduler runs on the GPU command thread, which is
    // where the hook's state lives.
    if (submit_hook_) {
        submit_hook_(submit_hook_user_);
    }
    // Every stream-lane byte this command buffer reads must be in place
    // before the queue submit.
    VideoCore::StreamCopyLane::Instance().DrainProducer();
    const u64 signal_value = work_semaphore.NextTick();
    // Cmdbuf rollover: mid-draw submits (stream wraparound flushes) must
    // synchronously invalidate all skip caches - a draw-entry token snapshot
    // cannot see this flush.
    VideoCore::Skipcache::Framework::Instance().InvalidateAll();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndSession();

    std::vector<vk::CommandBuffer> cmd_buffers;
    cmd_buffers.reserve(sessions.size() * 2);

    for (const auto& session : sessions) {
        if (session.upload) {
            cmd_buffers.push_back(session.upload);
        }
        cmd_buffers.push_back(session.primary);
    }
    sessions.clear();

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    std::array<vk::PipelineStageFlags, std::tuple_size_v<decltype(info.wait_semas)>>
        wait_stage_masks;
    wait_stage_masks.fill(vk::PipelineStageFlagBits::eAllCommands);

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = static_cast<u32>(cmd_buffers.size()),
        .pCommandBuffers = cmd_buffers.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    work_semaphore.Refresh();
    BeginSession();

    // Apply pending operations
    PopPendingOperations();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        // The storage is written before it is read: zeroing 128 KiB would buy nothing.
        return std::make_unique_for_overwrite<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::NextRecordChunk() {
    full_chunks.push_back(std::move(record_chunk));
    record_chunk = AcquireChunk();
}

void Scheduler::KickRecording(bool force) {
    if (!recorder_thread.joinable()) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < recorder_kick_bytes) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    ++(force ? recorder_forced_ : recorder_kicks_);
    const vk::CommandBuffer target = sessions.back().primary;
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            chunk->target = target;
            recorder_queue.push_back(std::move(chunk));
            ++handed_chunks_;
        }
        if (!record_chunk->Empty()) {
            record_chunk->target = target;
            recorder_queue.push_back(std::move(record_chunk));
            ++handed_chunks_;
            // The replacement comes from the same critical section: one lock per hand-over.
            if (!free_chunks.empty()) {
                record_chunk = std::move(free_chunks.back());
                free_chunks.pop_back();
            }
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = std::make_unique_for_overwrite<RecordChunk>();
    }
}

void Scheduler::SyncRecording() {
    if (!recorder_thread.joinable()) {
        return;
    }
    KickRecording(true);
    const auto idle = [this] {
        return done_chunks.load(std::memory_order_acquire) == handed_chunks_;
    };
    if (idle()) {
        return;
    }
    const u64 t0 = Common::FencedRDTSC();
    // The remainder is usually a few microseconds of work: a sleep and wake-up would cost
    // about as much again, so spin first and sleep only when the recorder is far behind.
    const auto spin_until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
    bool done = false;
    for (u32 spins = 1; !(done = idle()); ++spins) {
        Common::CpuPause();
        if (!(spins & 63) && std::chrono::steady_clock::now() >= spin_until) {
            break;
        }
    }
    if (!done) {
        std::unique_lock lk{recorder_mutex};
        recorder_idle_cv.wait(lk, idle);
        ++recorder_sync_blocked_;
    }
    recorder_sync_ticks_ += Common::FencedRDTSC() - t0;
}

void Scheduler::NoteSyncSite(const std::source_location& loc) {
    for (auto& site : sync_sites_) {
        if (site.count == 0) {
            site = {loc.file_name(), loc.line(), 1};
            return;
        }
        if (site.line == loc.line() && std::strcmp(site.file, loc.file_name()) == 0) {
            ++site.count;
            return;
        }
    }
    // ponytail: a full table drops later sites; the syncs total still counts them.
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:VkRecorder");
    // A spinning recorder on the GPU command thread's reserved core would take the time it saves.
    if (const u64 strip = Common::GetExclusionStripMask()) {
        Common::SetCurrentThreadAffinityMask(~strip);
    }
    // Spin briefly before sleeping: the next chunk usually follows within microseconds, and a
    // sleeping recorder costs the GPU command thread a wakeup syscall per kick. With few
    // hardware threads the spin would take time from the guest threads.
    const auto spin_time =
        std::chrono::microseconds(std::thread::hardware_concurrency() >= 12 ? 200 : 20);
    while (true) {
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            Common::CpuPause();
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return;
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
        }
        chunk->Execute(chunk->target);
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            done_chunks.fetch_add(1, std::memory_order_release);
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, Scheduler& scheduler) {
    // The dirty bits are read and cleared here; only the commands go through Record().
    const auto rec = [&scheduler](auto&& func) { scheduler.Record(std::move(func)); };
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        scheduler.ReserveRecordData(sizeof(viewports));
        const auto data = scheduler.RecordData(std::span<const vk::Viewport>{viewports});
        rec([data](vk::CommandBuffer cmdbuf) { cmdbuf.setViewportWithCount(data); });
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        scheduler.ReserveRecordData(sizeof(scissors));
        const auto data = scheduler.RecordData(std::span<const vk::Rect2D>{scissors});
        rec([data](vk::CommandBuffer cmdbuf) { cmdbuf.setScissorWithCount(data); });
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        rec([a0 = depth_test_enabled](vk::CommandBuffer cmdbuf) { cmdbuf.setDepthTestEnable(a0); });
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        rec([a0 = depth_write_enabled](vk::CommandBuffer cmdbuf) {
            cmdbuf.setDepthWriteEnable(a0);
        });
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        rec([a0 = depth_compare_op](vk::CommandBuffer cmdbuf) { cmdbuf.setDepthCompareOp(a0); });
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            rec([a0 = depth_bounds_test_enabled](vk::CommandBuffer cmdbuf) {
                cmdbuf.setDepthBoundsTestEnable(a0);
            });
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            rec([a0 = depth_bounds_min, a1 = depth_bounds_max](vk::CommandBuffer cmdbuf) {
                cmdbuf.setDepthBounds(a0, a1);
            });
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        rec([a0 = depth_bias_enabled](vk::CommandBuffer cmdbuf) { cmdbuf.setDepthBiasEnable(a0); });
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        rec([a0 = depth_bias_constant, a1 = depth_bias_clamp,
             a2 = depth_bias_slope](vk::CommandBuffer cmdbuf) { cmdbuf.setDepthBias(a0, a1, a2); });
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        rec([a0 = stencil_test_enabled](vk::CommandBuffer cmdbuf) {
            cmdbuf.setStencilTestEnable(a0);
        });
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            rec([a1 = stencil_front_ops.fail_op, a2 = stencil_front_ops.pass_op,
                 a3 = stencil_front_ops.depth_fail_op,
                 a4 = stencil_front_ops.compare_op](vk::CommandBuffer cmdbuf) {
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, a1, a2, a3, a4);
            });
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                rec([a1 = stencil_front_ops.fail_op, a2 = stencil_front_ops.pass_op,
                     a3 = stencil_front_ops.depth_fail_op,
                     a4 = stencil_front_ops.compare_op](vk::CommandBuffer cmdbuf) {
                    cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, a1, a2, a3, a4);
                });
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                rec([a1 = stencil_back_ops.fail_op, a2 = stencil_back_ops.pass_op,
                     a3 = stencil_back_ops.depth_fail_op,
                     a4 = stencil_back_ops.compare_op](vk::CommandBuffer cmdbuf) {
                    cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, a1, a2, a3, a4);
                });
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            rec([a1 = stencil_front_reference](vk::CommandBuffer cmdbuf) {
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, a1);
            });
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                rec([a1 = stencil_front_reference](vk::CommandBuffer cmdbuf) {
                    cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront, a1);
                });
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                rec([a1 = stencil_back_reference](vk::CommandBuffer cmdbuf) {
                    cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, a1);
                });
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            rec([a1 = stencil_front_write_mask](vk::CommandBuffer cmdbuf) {
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, a1);
            });
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                rec([a1 = stencil_front_write_mask](vk::CommandBuffer cmdbuf) {
                    cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront, a1);
                });
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                rec([a1 = stencil_back_write_mask](vk::CommandBuffer cmdbuf) {
                    cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, a1);
                });
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            rec([a1 = stencil_front_compare_mask](vk::CommandBuffer cmdbuf) {
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack, a1);
            });
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                rec([a1 = stencil_front_compare_mask](vk::CommandBuffer cmdbuf) {
                    cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront, a1);
                });
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                rec([a1 = stencil_back_compare_mask](vk::CommandBuffer cmdbuf) {
                    cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack, a1);
                });
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        rec([a0 = primitive_restart_enable](vk::CommandBuffer cmdbuf) {
            cmdbuf.setPrimitiveRestartEnable(a0);
        });
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        rec([a0 = rasterizer_discard_enable](vk::CommandBuffer cmdbuf) {
            cmdbuf.setRasterizerDiscardEnable(a0);
        });
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        rec([a0 = cull_mode](vk::CommandBuffer cmdbuf) { cmdbuf.setCullMode(a0); });
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        rec([a0 = front_face](vk::CommandBuffer cmdbuf) { cmdbuf.setFrontFace(a0); });
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        rec([v = blend_constants](vk::CommandBuffer cmdbuf) {
            cmdbuf.setBlendConstants(v.data());
        });
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskEnabled()) {
            rec([v = color_write_masks](vk::CommandBuffer cmdbuf) {
                cmdbuf.setColorWriteMaskEXT(0, v);
            });
        } else {
            ++color_write_mask_skips_;
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        rec([a0 = line_width](vk::CommandBuffer cmdbuf) { cmdbuf.setLineWidth(a0); });
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        const auto aspect = feedback_loop_enabled ? vk::ImageAspectFlagBits::eColor
                                                  : vk::ImageAspectFlagBits::eNone;
        rec([aspect](vk::CommandBuffer cmdbuf) {
            cmdbuf.setAttachmentFeedbackLoopEnableEXT(aspect);
        });
    }
}

} // namespace Vulkan

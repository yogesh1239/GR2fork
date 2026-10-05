// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/debug_print.h"
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Backend::SPIRV {

void EmitPrologue(EmitContext& ctx) {
    if (ctx.hw_stage == HwStage::Fragment) {
        ctx.DefineAmdPerVertexAttribs();
    }
    if (ctx.info.loads.Get(IR::Attribute::WorkgroupIndex)) {
        ctx.DefineWorkgroupIndex();
    }
    ctx.DefineBufferProperties();
}

void ConvertDepthMode(EmitContext& ctx) {
    const Id type{ctx.F32[1]};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id screen_depth{ctx.OpFMul(type, ctx.OpFAdd(type, z, w), ctx.Constant(type, 0.5f))};
    const Id vector{ctx.OpCompositeInsert(ctx.F32[4], screen_depth, position, 2u)};
    ctx.OpStore(ctx.output_position, vector);
}

void ConvertPositionToClipSpace(EmitContext& ctx) {
    ASSERT_MSG(!ctx.info.stores.GetAny(IR::Attribute::ViewportIndex),
               "Multi-viewport with shader clip space conversion not yet implemented.");

    const Id type{ctx.F32[1]};
    Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id x{ctx.OpCompositeExtract(type, position, 0u)};
    const Id y{ctx.OpCompositeExtract(type, position, 1u)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id xoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::XOffsetIndex))};
    const Id xoffset{ctx.OpLoad(type, xoffset_ptr)};
    const Id yoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::YOffsetIndex))};
    const Id yoffset{ctx.OpLoad(type, yoffset_ptr)};
    const Id xscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::XScaleIndex))};
    const Id xscale{ctx.OpLoad(type, xscale_ptr)};
    const Id yscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::YScaleIndex))};
    const Id yscale{ctx.OpLoad(type, yscale_ptr)};
    const Id vport_w =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_width / 2, 8_KB)));
    const Id wnd_x = ctx.OpFAdd(type, ctx.OpFMul(type, x, xscale), xoffset);
    const Id ndc_x = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_x, vport_w), ctx.Constant(type, 1.f));
    const Id vport_h =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_height / 2, 8_KB)));
    const Id wnd_y = ctx.OpFAdd(type, ctx.OpFMul(type, y, yscale), yoffset);
    const Id ndc_y = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_y, vport_h), ctx.Constant(type, 1.f));
    const Id vector{ctx.OpCompositeConstruct(ctx.F32[4], std::array<Id, 4>({ndc_x, ndc_y, z, w}))};
    ctx.OpStore(ctx.output_position, vector);
}

// FSR 4.1.1 object motion (runtime_info.h, MotionVectors), after bbport. The vertex shader
// stores its clip position in this frame's slot and loads the one of the previous frame (same
// draw, same vertex), both by buffer device address; PushData::motion names the slots. Disabled
// accesses are branched around: a shared scratch element would race between all the inactive
// vertex invocations.
static void EmitVertexMotion(EmitContext& ctx) {
    const Id u32_type = ctx.U32[1];
    const Id bool_type = ctx.U1[1];
    const Id position = ctx.OpLoad(ctx.F32[4], ctx.output_position);
    const auto push = [&](Id type, u32 index) {
        return ctx.OpLoad(type,
                          ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                            ctx.push_data_block, ctx.ConstU32(index)));
    };
    const Id store_base = push(u32_type, PushData::MotionIndex + 0);
    const Id load_base = push(u32_type, PushData::MotionIndex + 1);
    const Id vertices = push(u32_type, PushData::MotionIndex + 2);
    const Id first_vertex = push(u32_type, PushData::MotionIndex + 3);
    const Id first_instance = push(u32_type, PushData::MotionIndex + 4);
    const Id instances = push(u32_type, PushData::MotionIndex + 5);
    const Id positions = push(ctx.U64, PushData::MotionPositionsIndex);
    const Id vertex = ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.vertex_index), first_vertex);
    const Id instance = ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.instance_id), first_instance);
    const Id in_range = ctx.OpLogicalAnd(bool_type, ctx.OpULessThan(bool_type, vertex, vertices),
                                         ctx.OpULessThan(bool_type, instance, instances));
    const Id slot = ctx.OpIAdd(u32_type, vertex, ctx.OpIMul(u32_type, instance, vertices));
    // positions + (base + slot) * 16: one vec4 per vertex.
    const auto address = [&](Id base) {
        return ctx.OpIAdd(ctx.U64, positions,
                          ctx.OpIMul(ctx.U64,
                                     ctx.OpUConvert(ctx.U64, ctx.OpIAdd(u32_type, base, slot)),
                                     ctx.Constant(ctx.U64, u64{16})));
    };
    const Id do_store = ctx.OpLogicalAnd(
        bool_type, in_range, ctx.OpINotEqual(bool_type, store_base, ctx.u32_zero_value));
    const Id do_load = ctx.OpLogicalAnd(bool_type, in_range,
                                        ctx.OpINotEqual(bool_type, load_base, ctx.u32_zero_value));

    const Id store_label = ctx.OpLabel();
    const Id store_merge = ctx.OpLabel();
    ctx.OpSelectionMerge(store_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(do_store, store_label, store_merge);
    ctx.AddLabel(store_label);
    // Indexed draws may shade the same vertex more than once. Atomic component stores avoid
    // write/write races; all these invocations produce the same clip position.
    const Id store_address = address(store_base);
    const Id scalar_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, u32_type);
    const Id scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    for (u32 i = 0; i < 4; ++i) {
        const Id ptr = ctx.OpConvertUToPtr(
            scalar_ptr, ctx.OpIAdd(ctx.U64, store_address, ctx.Constant(ctx.U64, u64{i * 4})));
        const Id bits = ctx.OpBitcast(u32_type, ctx.OpCompositeExtract(ctx.F32[1], position, i));
        ctx.OpAtomicExchange(u32_type, ptr, scope, ctx.u32_zero_value, bits);
    }
    ctx.OpBranch(store_merge);
    ctx.AddLabel(store_merge);

    const Id load_label = ctx.OpLabel();
    const Id load_merge = ctx.OpLabel();
    ctx.OpSelectionMerge(load_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(do_load, load_label, load_merge);
    ctx.AddLabel(load_label);
    const Id vec_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.F32[4]);
    const Id loaded = ctx.OpLoad(ctx.F32[4], ctx.OpConvertUToPtr(vec_ptr, address(load_base)),
                                 spv::MemoryAccessMask::Aligned, 16u);
    ctx.OpBranch(load_merge);
    ctx.AddLabel(load_merge);
    Id previous = ctx.OpPhi(ctx.F32[4], position, store_merge, loaded, load_label);
    const Id valid = ctx.OpSelect(ctx.F32[1], do_load, ctx.Constant(ctx.F32[1], 1.0f),
                                  ctx.Constant(ctx.F32[1], 0.0f));
    previous = ctx.OpCompositeInsert(ctx.F32[4], valid, previous, 2u);
    ctx.OpStore(ctx.motion_out_cur, position);
    ctx.OpStore(ctx.motion_out_prev, previous);
}

// Writes previous NDC - current NDC (the units of GR2's velocity image), the depth of this fragment
// and the validity, which is the alpha the motion attachment blends with: an invalid fragment
// keeps what an earlier draw wrote there. The merge pass rejects a vector whose depth was covered.
static void EmitFragmentMotion(EmitContext& ctx) {
    const Id f32_type = ctx.F32[1];
    const Id bool_type = ctx.U1[1];
    const Id current = ctx.OpLoad(ctx.F32[4], ctx.motion_in_cur);
    const Id previous = ctx.OpLoad(ctx.F32[4], ctx.motion_in_prev);
    const auto component = [&](Id v, u32 i) { return ctx.OpCompositeExtract(f32_type, v, i); };
    const auto ndc = [&](Id clip, u32 i) {
        return ctx.OpFDiv(f32_type, component(clip, i), component(clip, 3u));
    };
    const Id zero = ctx.Constant(f32_type, 0.0f);
    const Id epsilon = ctx.Constant(f32_type, 1e-5f);
    // The flag is interpolated: a triangle with an unmatched vertex is invalid as a whole.
    const Id valid = ctx.OpLogicalAnd(
        bool_type,
        ctx.OpLogicalAnd(bool_type,
                         ctx.OpFOrdGreaterThan(bool_type, component(previous, 3u), epsilon),
                         ctx.OpFOrdGreaterThan(bool_type, component(current, 3u), epsilon)),
        ctx.OpFOrdGreaterThan(bool_type, component(previous, 2u), ctx.Constant(f32_type, 0.99f)));
    const Id dx = ctx.OpFSub(f32_type, ndc(previous, 0u), ndc(current, 0u));
    const Id dy = ctx.OpFSub(f32_type, ndc(previous, 1u), ndc(current, 1u));
    const Id depth = component(ctx.OpLoad(ctx.F32[4], ctx.frag_coord), 2u);
    // Zeros, not the quotients, when invalid: the blend multiplies by alpha 0, and NaN * 0 is NaN.
    ctx.OpStore(
        ctx.motion_frag_out,
        ctx.OpCompositeConstruct(
            ctx.F32[4],
            std::array<Id, 4>{ctx.OpSelect(f32_type, valid, dx, zero),
                              ctx.OpSelect(f32_type, valid, dy, zero), depth,
                              ctx.OpSelect(f32_type, valid, ctx.Constant(f32_type, 1.0f), zero)}));
}

void EmitEpilogue(EmitContext& ctx) {
    // Before the conversions below: the history holds the clip position the game wrote.
    if (Sirit::ValidId(ctx.motion_out_cur)) {
        EmitVertexMotion(ctx);
    }
    if (Sirit::ValidId(ctx.motion_frag_out)) {
        EmitFragmentMotion(ctx);
    }
    if (ctx.hw_stage == HwStage::Vertex &&
        ctx.runtime_info.hw.vs.emulate_depth_negative_one_to_one) {
        ConvertDepthMode(ctx);
    }
    if (ctx.hw_stage == HwStage::Vertex && ctx.runtime_info.hw.vs.clip_disable) {
        ConvertPositionToClipSpace(ctx);
    }
}

void EmitDiscard(EmitContext& ctx) {
    ctx.OpDemoteToHelperInvocationEXT();
}

void EmitDiscardCond(EmitContext& ctx, Id condition) {
    const Id kill_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, kill_label, merge_label);
    ctx.AddLabel(kill_label);
    ctx.OpDemoteToHelperInvocationEXT();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

void EmitEmitVertex(EmitContext& ctx) {
    ctx.OpEmitVertex();
}

void EmitEmitPrimitive(EmitContext& ctx) {
    ctx.OpEndPrimitive();
}

void EmitEmitVertex(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitEndPrimitive(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitDebugPrint(EmitContext& ctx, IR::Inst* inst, Id fmt, Id arg0, Id arg1, Id arg2, Id arg3) {
    IR::DebugPrintFlags flags = inst->Flags<IR::DebugPrintFlags>();
    std::array<Id, IR::DEBUGPRINT_NUM_FORMAT_ARGS> fmt_args = {arg0, arg1, arg2, arg3};
    auto fmt_args_span = std::span<Id>(fmt_args.begin(), fmt_args.begin() + flags.num_args);
    ctx.OpDebugPrintf(fmt, fmt_args_span);
}

Id EmitMemtime(EmitContext& ctx) {
    if (ctx.profile.supports_shader_subgroup_clock) {
        return ctx.OpReadClockKHR(ctx.U64, ctx.ConstU32(std::to_underlying(spv::Scope::Subgroup)));
    } else {
        return ctx.Constant(ctx.U64, 1U);
    }
}

} // namespace Shader::Backend::SPIRV

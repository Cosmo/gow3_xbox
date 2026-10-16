// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"

namespace Vulkan {

// Dozen implements these Vulkan dynamic states with D3D12 PSO variants at draw time.
// Keep their effective values in the pipeline key so cache warm-up/async compilation
// creates the real PSO instead of postponing its compilation to the first particle draw.
struct BakedGraphicsState {
    u32 enabled{};
    u32 depth_test{}, depth_write{}, depth_compare{}, depth_bounds{};
    u32 stencil_test{};
    vk::StencilOpState front{}, back{};
    u32 depth_bias{};
    float bias_constant{}, bias_clamp{}, bias_slope{};
    u32 cull_mode{}, front_face{}, discard{}, restart{};
};

inline BakedGraphicsState GetBakedGraphicsState(const AmdGpu::Regs& regs,
                                               bool list_restart, bool patch_restart,
                                               bool depth_bounds, bool raw_slope) {
    BakedGraphicsState state{};
    state.enabled = 1;
    const auto effective = GetEffectiveDepthStencilState(regs);
    state.depth_test = effective.depth_test_enable;
    state.depth_write = effective.depth_write_enable && !regs.depth_render_control.depth_clear_enable;
    state.depth_compare = u32(state.depth_test ? LiverpoolToVK::CompareOp(regs.depth_control.depth_func)
                                              : vk::CompareOp::eAlways);
    state.depth_bounds = effective.depth_bounds_enable && depth_bounds;
    state.stencil_test = effective.stencil_test_enable;
    if (state.stencil_test) {
        const auto& sc = regs.stencil_control;
        const auto make = [&](bool back) {
            const auto ref = back ? regs.stencil_ref_back : regs.stencil_ref_front;
            return vk::StencilOpState{
                .failOp = LiverpoolToVK::StencilOp(back ? sc.stencil_fail_back : sc.stencil_fail_front),
                .passOp = LiverpoolToVK::StencilOp(back ? sc.stencil_zpass_back : sc.stencil_zpass_front),
                .depthFailOp = LiverpoolToVK::StencilOp(back ? sc.stencil_zfail_back : sc.stencil_zfail_front),
                .compareOp = LiverpoolToVK::CompareOp(back ? regs.depth_control.stencil_bf_func
                                                         : regs.depth_control.stencil_ref_func),
                .compareMask = ref.stencil_mask,
                .writeMask = regs.depth_render_control.stencil_clear_enable ? 0u : ref.stencil_write_mask,
                // Reference stays dynamic: D3D12 supports it without compiling another PSO.
            };
        };
        state.front = make(false);
        state.back = regs.depth_control.backface_enable ? make(true) : state.front;
    }
    state.depth_bias = regs.polygon_control.NeedsBias();
    if (state.depth_bias) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        state.bias_constant = front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset;
        state.bias_clamp = regs.poly_offset.depth_bias;
        const float slope = front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale;
        state.bias_slope = raw_slope ? slope : slope / 16.f;
    }
    state.cull_mode = u32(LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                             ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                             : vk::CullModeFlagBits::eNone);
    state.front_face = u32(LiverpoolToVK::FrontFace(regs.polygon_control.front_face));
    state.discard = regs.clipper_control.dx_rasterization_kill;
    const auto topology = LiverpoolToVK::PrimitiveType(regs.primitive_type);
    const bool list = topology == vk::PrimitiveTopology::ePointList ||
                      topology == vk::PrimitiveTopology::eLineList ||
                      topology == vk::PrimitiveTopology::eTriangleList ||
                      topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
                      topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    const bool patch = regs.primitive_type == AmdGpu::PrimitiveType::PatchPrimitive ||
                       regs.primitive_type == AmdGpu::PrimitiveType::QuadList ||
                       regs.primitive_type == AmdGpu::PrimitiveType::RectList;
    state.restart = (regs.enable_primitive_restart & 1) && (list_restart || !list) &&
                    (patch_restart || !patch);
    return state;
}

inline bool IsBakedDynamicState(vk::DynamicState state) {
    switch (state) {
    case vk::DynamicState::eDepthTestEnable:
    case vk::DynamicState::eDepthWriteEnable:
    case vk::DynamicState::eDepthCompareOp:
    case vk::DynamicState::eDepthBoundsTestEnable:
    case vk::DynamicState::eDepthBiasEnable:
    case vk::DynamicState::eDepthBias:
    case vk::DynamicState::eStencilTestEnable:
    case vk::DynamicState::eStencilCompareMask:
    case vk::DynamicState::eStencilWriteMask:
    case vk::DynamicState::eStencilOp:
    case vk::DynamicState::eCullMode:
    case vk::DynamicState::eFrontFace:
    case vk::DynamicState::eRasterizerDiscardEnable:
    case vk::DynamicState::ePrimitiveRestartEnable:
        return true;
    default:
        return false;
    }
}
} // namespace Vulkan

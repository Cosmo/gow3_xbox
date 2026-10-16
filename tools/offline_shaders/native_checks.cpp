// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_shader_inputs.h"
#include "gow3_shader_template.h"
#include "gow3_template_cache.h"
#include "video_core/renderer_vulkan/vk_baked_state.h"
#include <memory>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <spirv/unified1/spirv.hpp11>

int RunOfflineShaderChecks() {
    using namespace Gow3Precompile;
    const auto require = [](bool condition) {
        if (!condition)
            throw std::runtime_error("Native shader preparation check failed");
    };
    {
        const auto check = [](bool condition) {
            if (!condition) throw std::runtime_error("Baked graphics state check failed");
        };
        auto regs = std::make_unique<AmdGpu::Regs>();
        regs->primitive_type = AmdGpu::PrimitiveType::TriangleList;
        const auto state = [&] { return Vulkan::GetBakedGraphicsState(*regs, false, false, true, false); };
        const auto empty = state();
        regs->depth_control.depth_func = AmdGpu::CompareFunc::Less;
        const auto unused_compare = state();
        check(std::memcmp(&empty, &unused_compare, sizeof(empty)) == 0);
        regs->depth_buffer.z_read_base = regs->depth_buffer.z_write_base = 1;
        regs->depth_buffer.z_info.format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
        regs->depth_control.depth_enable = 1;
        regs->depth_control.depth_write_enable = 1;
        check(state().depth_test && state().depth_write);
        regs->depth_render_control.depth_clear_enable = 1;
        check(!state().depth_write);
        regs->depth_buffer.stencil_read_base = regs->depth_buffer.stencil_write_base = 1;
        regs->depth_buffer.stencil_info.format = AmdGpu::DepthBuffer::StencilFormat::Stencil8;
        regs->depth_control.stencil_enable = 1;
        regs->depth_control.stencil_ref_func = AmdGpu::CompareFunc::Less;
        regs->stencil_ref_front.stencil_mask = 0x7f;
        regs->stencil_ref_front.stencil_write_mask = 0x3f;
        check(state().stencil_test && state().front.compareMask == 0x7f);
        check(state().front == state().back);
        regs->depth_control.backface_enable = 1;
        regs->depth_control.stencil_bf_func = AmdGpu::CompareFunc::Greater;
        regs->stencil_ref_back.stencil_write_mask = 0x1f;
        check(state().back.writeMask == 0x1f && state().front.writeMask == 0x3f);
        regs->depth_render_control.stencil_clear_enable = 1;
        check(state().front.writeMask == 0 && state().back.writeMask == 0);
        regs->polygon_control.enable_polygon_offset_front = 1;
        regs->poly_offset.front_scale = 16.f;
        check(state().bias_slope == 1.f);
        check(Vulkan::GetBakedGraphicsState(*regs, false, false, true, true).bias_slope == 16.f);
        regs->primitive_type = AmdGpu::PrimitiveType::RectList;
        regs->enable_primitive_restart = 1;
        check(!state().restart);
        check(Vulkan::IsBakedDynamicState(vk::DynamicState::eDepthCompareOp));
        check(!Vulkan::IsBakedDynamicState(vk::DynamicState::eStencilReference));
        check(!Vulkan::IsBakedDynamicState(vk::DynamicState::eViewportWithCount));
        std::puts("PASS: baked graphics state normalization, depth/stencil, bias and restart");
    }
    const std::array<u32, 4> usages = {0x0002011b, 0x03040200, 0x000c0201, 0x00100002};
    AssetInputs inputs(usages, 3);
    require(!inputs.Parametric());
    const auto user = inputs.UserData();
    AmdGpu::Image image;
    AmdGpu::Sampler sampler;
    std::memcpy(&image, user.data() + 4, sizeof(image));
    std::memcpy(&sampler, user.data() + 12, sizeof(sampler));
    require(image.Valid() && image.GetType() == AmdGpu::ImageType::Color2D);
    require(sampler.force_unnormalized && sampler.force_degamma);
    const u64 extended = u64(user[2]) | (u64(user[3]) << 32);
    require(inputs.Read32(extended) == 1);
    for (u64 invalid : {extended + 1, extended + 1024, u64(0xffffffffffffffffULL)}) {
        bool refused = false;
        try {
            inputs.Read32(invalid);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        require(refused);
    }
    bool refused = false;
    try {
        AssetInputs invalid(std::array<u32, 1>{0x030f0200});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused);
    const auto fetch = AssetFetch(std::array<u32, 2>{0x00040400, 0x00040802},
                                  std::array<u32, 2>{0x12, 0x10020017});
    require(fetch.fetch && fetch.fetch->attributes.size() == 2);
    require(fetch.fetch->attributes[1].dword_offset == 8 &&
            fetch.fetch->attributes[1].dest_vgpr == 8);

    // Prepared walkers follow the current tables and recover from a null table.
    std::array<u32, 4> child{0, 456, 0, 0}, table{0, 0, 123, 0};
    const auto child_address = reinterpret_cast<u64>(child.data());
    std::memcpy(table.data(), &child_address, 8);
    std::array<u32, 16> live_user{};
    const auto table_address = reinterpret_cast<u64>(table.data());
    std::memcpy(live_user.data(), &table_address, 8);
    const Shader::SrtReadPlan plan{{{0}, 2, 16}, {{0, 0}, 1, 17}};
    auto walker = Shader::RegisterSrtReadPlan(plan, 18, 2);
    std::array<u32, 18> flat{};
    walker.walker_func(live_user.data(), flat.data());
    require(flat[16] == 123 && flat[17] == 456);
    live_user[0] = live_user[1] = 0;
    walker.walker_func(live_user.data(), flat.data());
    require(flat[16] == 0 && flat[17] == 0);
    std::memcpy(live_user.data(), &table_address, 8);
    child[1] = 789;
    walker.walker_func(live_user.data(), flat.data());
    require(flat[16] == 123 && flat[17] == 789);
    refused = false;
    try {
        Shader::RegisterSrtReadPlan(plan, 19, 2);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused);

    const std::vector<u32> source = {0x07230203,
                                     0x00010500,
                                     0,
                                     10,
                                     0,
                                     (4u << 16) | u32(spv::Op::OpDecorate),
                                     3,
                                     u32(spv::Decoration::SpecId),
                                     Shader::OfflineSlot::Normalize,
                                     (4u << 16) | u32(spv::Op::OpTypeInt),
                                     1,
                                     32,
                                     0,
                                     (4u << 16) | u32(spv::Op::OpSpecConstant),
                                     1,
                                     3,
                                     0};
    std::vector<u32> result;
    require(FreezeTemplate(
        source,
        [](u32 slot) -> std::optional<u32> {
            return slot == Shader::OfflineSlot::Normalize ? std::optional<u32>(1) : std::nullopt;
        },
        result));
    require(result.size() == source.size() - 4 && result.back() == 1 &&
            (result[result.size() - 4] & 0xffff) == u32(spv::Op::OpConstant));
    auto sentinel = result;
    require(!FreezeTemplate(
                source, [](u32) -> std::optional<u32> { return {}; }, result) &&
            result == sentinel);
    auto truncated = source;
    truncated.pop_back();
    require(!FreezeTemplate(truncated, [](u32) -> std::optional<u32> { return 1; }, result));
    auto invalid = source;
    invalid[5] = 0;
    require(!FreezeTemplate(invalid, [](u32) -> std::optional<u32> { return 1; }, result));

    // The library accepts only its code/runtime/profile and static descriptor
    // layout. Deferred presence/stride values resolve from the current snapshot.
    const auto directory = std::filesystem::temp_directory_path() /
        ("gow3-template-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Shader::Profile profile;
    Shader::RuntimeInfo runtime{};
    runtime.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    runtime.props.num_user_data = 16;
    Shader::Info baseline;
    baseline.hw_stage = runtime.hw_stage;
    baseline.sw_stage = runtime.sw_stage;
    baseline.pgm_hash = 123;
    baseline.pgm_base = 0;
    baseline.flattened_ud_buf.resize(16);
    auto buffer = AmdGpu::Buffer::Placeholder(4);
    buffer.stride = 16;
    std::memcpy(baseline.flattened_ud_buf.data(), &buffer, sizeof(buffer));
    Shader::BufferResource resource{};
    resource.sharp_fetch.load_mask = 15;
    resource.sharp_fetch.offsets = {0, 1, 2, 3};
    baseline.buffers.push_back(resource);
    auto presence_source = source;
    presence_source[8] = Shader::OfflineSlot::BufferPresent;
    {
        TemplateWriter writer(directory, profile, 456);
        const auto location = writer.Write(789, 1011, baseline, presence_source);
        writer.Alias(location, 123);
        writer.Alias(location, 124);
        writer.Finish();
    }
    u32 entries = 0;
    require(TemplatesReady(directory, profile, 456, entries) && entries == 2);
    require(!TemplatesReady(directory, profile, 457, entries));
    auto current = baseline;
    current.offline_descriptor_snapshot = true;
    require(TryTemplate(directory, 1011, 789, current, runtime, profile, result) && result.back() == 1);
    current = baseline;
    current.offline_descriptor_snapshot = true;
    current.pgm_hash = 124;
    require(TryTemplate(directory, 1011, 789, current, runtime, profile, result));
    require(!TryTemplate(directory, 1012, 789, current, runtime, profile, result));
    require(!TryTemplate(directory, 1011, 790, current, runtime, profile, result));
    auto wrong_profile = profile;
    wrong_profile.needs_buffer_offsets = !profile.needs_buffer_offsets;
    require(!TryTemplate(directory, 1011, 789, current, runtime, wrong_profile, result));
    buffer = AmdGpu::Buffer::Null();
    std::memcpy(current.flattened_ud_buf.data(), &buffer, sizeof(buffer));
    require(TryTemplate(directory, 1011, 789, current, runtime, profile, result) && result.back() == 0);
    buffer = AmdGpu::Buffer::Placeholder(4);
    buffer.swizzle_enable = 1;
    std::memcpy(current.flattened_ud_buf.data(), &buffer, sizeof(buffer));
    require(!TryTemplate(directory, 1011, 789, current, runtime, profile, result));
    {
        std::fstream corrupt(directory / "templates.gtp", std::ios::binary | std::ios::in | std::ios::out);
        corrupt.put(0);
    }
    current = baseline;
    current.offline_descriptor_snapshot = true;
    require(!TryTemplate(directory, 1011, 789, current, runtime, profile, result));
    std::filesystem::remove(directory / "templates.gtp");
    std::filesystem::remove(directory / "templates.gti");
    std::filesystem::remove(directory);
    std::puts("PASS: bounded asset descriptors, fetch semantics, template "
              "constants and invalid-input fallback");
    return 0;
}

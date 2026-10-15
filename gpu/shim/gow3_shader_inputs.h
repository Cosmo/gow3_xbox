// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/offline_memory.h"
#include "video_core/amdgpu/resource.h"
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>

namespace Gow3Precompile {
// Explicit descriptor variant: ordinary float 2D images, normalized samplers,
// and unformatted constant buffers. Runtime lookup must check specialization
// before using an output prepared for this variant.
class AssetInputs final : public Shader::OfflineMemory {
  public:
    explicit AssetInputs(std::span<const u32> usages, u32 sampler_flags = 0, bool parametric = false)
        : parametric(parametric) {
        AmdGpu::Image image = AmdGpu::Image::Null(true);
        image.base_address = 1;
        AmdGpu::Buffer buffer = AmdGpu::Buffer::Placeholder(65536);
        buffer.data_format = u32(AmdGpu::DataFormat::Format32);
        buffer.stride = 16;
        buffer.num_format = u32(AmdGpu::NumberFormat::Float);
        buffer.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        buffer.dst_sel_y = u32(AmdGpu::CompSwizzle::Green);
        buffer.dst_sel_z = u32(AmdGpu::CompSwizzle::Blue);
        buffer.dst_sel_w = u32(AmdGpu::CompSwizzle::Alpha);
        for (size_t at = 0; at < resources.size(); at += 8)
            std::memcpy(resources.data() + at, &image, sizeof(image));
        AmdGpu::Sampler sampler{};
        sampler.force_unnormalized.Assign(sampler_flags & 1);
        sampler.force_degamma.Assign((sampler_flags >> 1) & 1);
        for (size_t at = 0; at < samplers.size(); at += 4)
            std::memcpy(samplers.data() + at, &sampler, sizeof(sampler));
        for (u32 usage : usages) {
            const u32 type = usage & 255, reg = (usage >> 16) & 255, flags = usage >> 24;
            switch (type) {
            case 0: // immediate V# or T#
                if (flags & 2)
                    Put(reg, image);
                else
                    Put(reg, buffer);
                break;
            case 1:
                Put(reg, sampler);
                break;
            case 2:
                Put(reg, buffer);
                break;
            case 0x13:
                Put(reg, ResourceBase);
                break;
            case 0x15:
                Put(reg, SamplerBase);
                break;
            case 0x1b:
                Put(reg, ExtendedBase);
                break;
            case 0x12:
                break; // Fetch supplied explicitly from the asset semantics.
            case 0x17:
                Put(reg, VertexBase);
                break;
            default:
                throw std::runtime_error("Unsupported asset resource usage type");
            }
        }
    }
    std::span<const u32> UserData() const { return user; }
    bool Parametric() const override { return parametric; }
    u32 Read32(u64 address) const override {
        if (address % 4)
            throw std::runtime_error("Unaligned offline descriptor read");
        auto read = [&](u64 base, std::span<const u32> words) -> const u32* {
            if (address >= base && (address - base) / 4 < words.size())
                return &words[(address - base) / 4];
            return nullptr;
        };
        if (auto p = read(ExtendedBase, extended))
            return *p;
        if (auto p = read(ResourceBase, resources))
            return *p;
        if (auto p = read(SamplerBase, samplers))
            return *p;
        if (auto p = read(VertexBase, vertices))
            return *p;
        throw std::runtime_error("Offline descriptor read outside supplied tables");
    }

  private:
    bool parametric;
    static constexpr u64 ExtendedBase = 0x10000, ResourceBase = 0x20000, SamplerBase = 0x30000,
                         VertexBase = 0x40000;
    std::array<u32, 16> user{};
    std::array<u32, 256> extended{};
    std::array<u32, 2048> resources{};
    std::array<u32, 1024> samplers{};
    std::array<u32, 1024> vertices{};
    template <class T> void Put(u32 reg, const T& value) {
        auto words = reg < 16 ? std::span<u32>(user) : std::span<u32>(extended);
        const u32 offset = reg < 16 ? reg : reg - 16;
        if (offset + sizeof(T) / 4 > words.size())
            throw std::runtime_error("Asset descriptor crosses register bounds");
        std::memcpy(words.data() + offset, &value, sizeof(value));
    }
};

inline Shader::Gcn::FetchShaderSnapshot AssetFetch(std::span<const u32> semantics,
                                                 std::span<const u32> usages, bool skinned = false) {
    Shader::Gcn::FetchShaderSnapshot result;
    result.fetch.emplace();
    result.fetch->size = semantics.size() * 12 + 12;
    u32 table_register = 0;
    bool found = false;
    for (u32 usage : usages)
        if ((usage & 255) == 0x17) {
            table_register = (usage >> 16) & 255;
            found = true;
        }
    if (!found)
        throw std::runtime_error("Vertex asset is missing its buffer-table register");
    for (u32 packed : semantics) {
        const u32 semantic = packed & 255, vgpr = (packed >> 8) & 255,
                  components = (packed >> 16) & 255;
        if (packed >> 24 || !components || components > 4 || semantic >= 64 || vgpr > 252)
            throw std::runtime_error("Unsupported vertex input semantic");
        Shader::Gcn::VertexAttribute attribute{};
        attribute.semantic = result.fetch->attributes.size();
        attribute.dest_vgpr = vgpr;
        attribute.num_elements = components;
        attribute.sgpr_base = table_register;
        attribute.dword_offset = semantic * 4;
        result.fetch->attributes.push_back(attribute);
        auto buffer = AmdGpu::Buffer::Placeholder(65536);
        buffer.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
        buffer.num_format = u32(AmdGpu::NumberFormat::Float);
        buffer.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        buffer.dst_sel_y =
            u32(components >= 2 ? AmdGpu::CompSwizzle::Green : AmdGpu::CompSwizzle::Zero);
        buffer.dst_sel_z =
            u32(components >= 3 ? AmdGpu::CompSwizzle::Blue : AmdGpu::CompSwizzle::Zero);
        buffer.dst_sel_w =
            u32(components == 4 ? AmdGpu::CompSwizzle::Alpha : AmdGpu::CompSwizzle::One);
        // Explicit common mesh-layout variant. Other packed layouts remain separate.
        if (semantic == 0 && components == 4)
            buffer.dst_sel_w = u32(AmdGpu::CompSwizzle::One);
        if (semantic >= 14 && !skinned) {
            buffer.dst_sel_x = buffer.dst_sel_y = buffer.dst_sel_z = u32(AmdGpu::CompSwizzle::Zero);
            buffer.dst_sel_w = u32(AmdGpu::CompSwizzle::One);
        }
        if(semantic==15&&skinned) buffer.num_format=u32(AmdGpu::NumberFormat::Uint);
        result.buffers.push_back(buffer);
    }
    return result;
}
} // namespace Gow3Precompile

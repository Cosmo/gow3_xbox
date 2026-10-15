// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_shader_template.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include <spirv/unified1/spirv.hpp11>
#include <unordered_map>

namespace Gow3Precompile {
bool FreezeTemplate(std::span<const u32> input, const ConstantResolver &resolve,
                    std::vector<u32> &output) {
  if (input.size() < 5 || input[0] != 0x07230203)
    return false;
  std::unordered_map<u32, u32> slots;
  for (size_t at = 5; at < input.size();) {
    const u32 size = input[at] >> 16;
    if (!size || size > input.size() - at)
      return false;
    if ((input[at] & 0xffff) == u32(spv::Op::OpDecorate) && size == 4 &&
        input[at + 2] == u32(spv::Decoration::SpecId)) {
      if (!slots.emplace(input[at + 1], input[at + 3]).second)
        return false;
    }
    at += size;
  }
  std::vector<u32> checked(input.begin(), input.begin() + 5);
  for (size_t at = 5; at < input.size();) {
    const u32 size = input[at] >> 16;
    const auto opcode = static_cast<spv::Op>(input[at] & 0xffff);
    if (opcode == spv::Op::OpDecorate && size == 4 &&
        input[at + 2] == u32(spv::Decoration::SpecId)) {
      at += size;
      continue;
    }
    if (opcode == spv::Op::OpSpecConstant) {
      if (size != 4)
        return false;
      const auto slot = slots.find(input[at + 2]);
      if (slot == slots.end())
        return false;
      const auto value = resolve(slot->second);
      if (!value)
        return false;
      checked.insert(checked.end(), {(4u << 16) | u32(spv::Op::OpConstant),
                                     input[at + 1], input[at + 2], *value});
      slots.erase(slot);
    } else if (opcode == spv::Op::OpSpecConstantTrue ||
               opcode == spv::Op::OpSpecConstantFalse ||
               opcode == spv::Op::OpSpecConstantComposite ||
               opcode == spv::Op::OpSpecConstantOp) {
      return false;
    } else {
      checked.insert(checked.end(), input.begin() + at,
                     input.begin() + at + size);
    }
    at += size;
  }
  if (!slots.empty())
    return false;
  output = std::move(checked);
  return true;
}

std::optional<u32> DescriptorConstant(u32 slot, const Shader::Info &info) {
  using namespace Shader;
  if (slot >= OfflineSlot::Normalize &&
      slot < OfflineSlot::Normalize + NUM_SAMPLERS) {
    const u32 index = slot - OfflineSlot::Normalize;
    if (index >= info.samplers.size())
      return {};
    return u32(info.samplers[index].GetSharp(info).force_unnormalized);
  }
  if (slot >= OfflineSlot::Gamma &&
      slot < OfflineSlot::Gamma + NUM_SAMPLERS * 64) {
    const u32 index = slot - OfflineSlot::Gamma, sampler = index / 64,
              image = index % 64;
    if (sampler >= info.samplers.size() || image >= info.images.size())
      return {};
    return u32(info.samplers[sampler].GetSharp(info).force_degamma &&
               info.images[image].GetSharp(info).GetNumberFmt() !=
                   AmdGpu::NumberFormat::Srgb);
  }
  if (slot >= OfflineSlot::Stride && slot < OfflineSlot::Stride + NUM_BUFFERS) {
    const u32 index = slot - OfflineSlot::Stride;
    if (index >= info.buffers.size())
      return {};
    return info.buffers[index].GetSharp(info).GetStride();
  }
  if (slot >= OfflineSlot::BufferPresent &&
      slot < OfflineSlot::BufferPresent + NUM_BUFFERS) {
    const u32 index = slot - OfflineSlot::BufferPresent;
    if (index >= info.buffers.size())
      return {};
    const auto &desc = info.buffers[index];
    const auto sharp = desc.GetSharp(info);
    return u32(desc.IsSpecial() || (sharp && sharp.GetSize()));
  }
  if (slot >= OfflineSlot::ImagePresent &&
      slot < OfflineSlot::ImagePresent + NUM_IMAGES) {
    const u32 index = slot - OfflineSlot::ImagePresent;
    if (index >= info.images.size())
      return {};
    return u32(bool(info.images[index].GetSharp(info)));
  }
  if (slot >= OfflineSlot::Swizzle && slot < OfflineSlot::Swizzle + 32 * 4) {
    const u32 index = (slot - OfflineSlot::Swizzle) / 4,
              component = (slot - OfflineSlot::Swizzle) % 4;
    const auto fetch = Gcn::ParseFetchShader(info);
    if (!fetch || index >= fetch->attributes.size())
      return {};
    const auto mapping = fetch->attributes[index].GetSharp(info).DstSelect();
    const std::array<u32, 4> values = {u32(mapping.r), u32(mapping.g),
                                       u32(mapping.b), u32(mapping.a)};
    return values[component];
  }
  return {};
}
} // namespace Gow3Precompile

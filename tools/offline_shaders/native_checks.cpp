// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_shader_inputs.h"
#include "gow3_shader_template.h"
#include <cstdio>
#include <spirv/unified1/spirv.hpp11>

int RunOfflineShaderChecks() {
  using namespace Gow3Precompile;
  const auto require = [](bool condition) {
    if (!condition)
      throw std::runtime_error("Native shader preparation check failed");
  };
  const std::array<u32, 4> usages = {0x0002011b, 0x03040200, 0x000c0201,
                                     0x00100002};
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
  for (u64 invalid :
       {extended + 1, extended + 1024, u64(0xffffffffffffffffULL)}) {
    bool refused = false;
    try {
      inputs.Read32(invalid);
    } catch (const std::runtime_error &) {
      refused = true;
    }
    require(refused);
  }
  bool refused = false;
  try {
    AssetInputs invalid(std::array<u32, 1>{0x030f0200});
  } catch (const std::runtime_error &) {
    refused = true;
  }
  require(refused);
  const auto fetch = AssetFetch(std::array<u32, 2>{0x00040400, 0x00040802},
                                std::array<u32, 2>{0x12, 0x10020017});
  require(fetch.fetch && fetch.fetch->attributes.size() == 2);
  require(fetch.fetch->attributes[1].dword_offset == 8 &&
          fetch.fetch->attributes[1].dest_vgpr == 8);

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
        return slot == Shader::OfflineSlot::Normalize ? std::optional<u32>(1)
                                                      : std::nullopt;
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
  require(!FreezeTemplate(
      truncated, [](u32) -> std::optional<u32> { return 1; }, result));
  auto invalid = source;
  invalid[5] = 0;
  require(!FreezeTemplate(
      invalid, [](u32) -> std::optional<u32> { return 1; }, result));
  std::puts("PASS: bounded asset descriptors, fetch semantics, template "
            "constants and invalid-input fallback");
  return 0;
}

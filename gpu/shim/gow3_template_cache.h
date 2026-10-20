// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include <filesystem>
#include <memory>
namespace Shader::Gcn { struct FetchShaderSnapshot; }

namespace Gow3Precompile {
struct TemplateLocation {
    u64 hash{}, code_hash{}, runtime_key{}, offset{}, checksum{};
    u32 bytes{}, reserved{};
};
class TemplateWriter {
  public:
    TemplateWriter(const std::filesystem::path& cache, const Shader::Profile& profile, u64 library);
    ~TemplateWriter();
    TemplateLocation Write(u64 code_hash, u64 runtime_key, const Shader::Info& info,
                           std::span<const u32> spirv,
                           const Shader::Gcn::FetchShaderSnapshot* fetch = nullptr);
    void Alias(TemplateLocation location, u64 original_hash);
    void Finish();

  private:
    struct State;
    std::unique_ptr<State> state;
};
bool TemplatesReady(const std::filesystem::path& cache, const Shader::Profile& profile, u64 library,
                    u32& entries);
std::vector<Shader::Backend::Bindings> TemplateBindingVariants(const std::filesystem::path& cache,
    u64 hash, u64 code_hash, const Shader::Profile& profile);
bool TryTemplate(const std::filesystem::path& cache, u64 runtime_key, u64 code_hash,
                 Shader::Info& info, const Shader::RuntimeInfo& runtime,
                 const Shader::Profile& profile, std::vector<u32>& spirv,
                 const Shader::Gcn::FetchShaderSnapshot* captured_fetch = nullptr);
} // namespace Gow3Precompile

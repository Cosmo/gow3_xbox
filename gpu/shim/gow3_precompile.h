// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Gow3Precompile {
struct Progress {
    bool scanning = true;
    u32 done = 0, total = 0, supported = 0, skipped = 0, failed = 0;
};
struct Summary {
    u32 supported = 0, prepared_entries = 0, distinct_programs = 0, skipped = 0;
    u32 compiled = 0, already_cached = 0, failed = 0, archives = 0;
};
using Update = std::function<void(const Progress&)>;
std::filesystem::path CacheDirectory();
Summary Prepare(const std::filesystem::path& dump, const std::filesystem::path& cache,
                const Shader::Profile& profile, const Update& update);
bool TryLoad(Shader::Info& info, const Shader::RuntimeInfo& runtime, std::span<const u32> code,
             const Shader::Profile& profile, std::vector<u32>& spirv);
bool TryLoadAt(const std::filesystem::path& cache, Shader::Info& info,
               const Shader::RuntimeInfo& runtime, std::span<const u32> code,
               const Shader::Profile& profile, std::vector<u32>& spirv);
} // namespace Gow3Precompile

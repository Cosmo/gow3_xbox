// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_precompile.h"
#include "gow3_shader_inputs.h"
#include "gow3_template_cache.h"
#include "common/path_util.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/regs_depth.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>
#ifdef GOW3_UWP
#include <windows.h>
extern "C" HANDLE gow3_uwp_find(const char*, WIN32_FIND_DATAA*);
extern "C" BOOL gow3_uwp_find_next(HANDLE, WIN32_FIND_DATAA*);
extern "C" FILE* gow3_uwp_fopen(const char*, const char*);
#endif

namespace Gow3Precompile {
namespace {
namespace fs = std::filesystem;
using namespace Shader;
u32 U32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
u64 U64(const u8* p) {
    u64 v;
    std::memcpy(&v, p, 8);
    return v;
}
u16 U16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}
struct Candidate {
    u64 hash{}, code_hash{}, signature{};
    u64 fragment{};
    std::array<u32, 13> regs{};
    std::vector<u32> parameters, usages, code;
};
struct Program {
    size_t start{}, footer{};
    u64 hash{};
    u32 bytes{}, type{}, usages{};
};
std::optional<Program> ProgramAt(std::span<const u8> data, size_t at) {
    if (at + 8 > data.size() || U32(data.data() + at) != 0xbeeb03ff)
        return {};
    const u64 delta = (u64(U32(data.data() + at + 4)) + 1) * 8;
    if (delta < 8 || delta > 65536 || at + delta + 28 > data.size())
        return {};
    const auto* footer = data.data() + at + delta;
    if (std::memcmp(footer, "OrbShdr", 7))
        return {};
    const u32 length = U32(footer + 8) >> 8;
    if (!length || length % 4 || length > delta)
        return {};
    return Program{at,     at + size_t(delta),          U64(footer + 16),
                   length, (U32(footer + 8) >> 2) & 15, footer[13]};
}
bool VertexExports(std::span<const u8> data, const Program& p,
                   std::unordered_map<u32, u32>& exports, Candidate* candidate = nullptr) {
    for (size_t size = 32; size <= std::min(p.start, size_t(1244)); size += 4) {
        const auto* h = data.data() + p.start - size;
        if (U32(h) != size + 8 || U32(h + 4) != 0xffffffff)
            continue;
        const u32 counts = U32(h + 28), inputs = counts & 255, outputs = (counts >> 8) & 255;
        if (counts >> 16 || inputs > 32 || outputs > 32 ||
            size != 32 + p.usages * 4 + inputs * 4 + ((outputs * 2 + 3) & ~3))
            continue;
        exports.clear();
        const auto* table = h + 32 + p.usages * 4 + inputs * 4;
        for (u32 i = 0; i < outputs; ++i) {
            const u16 value = U16(table + i * 2);
            if (!exports.emplace(value & 255, value >> 8).second)
                return false;
        }
        if (candidate) {
            if (U32(h + 24)) return false;
            candidate->hash = p.hash;
            std::memcpy(candidate->regs.data(), h, 32);
            candidate->usages.resize(p.usages);
            std::memcpy(candidate->usages.data(), h + 32, p.usages * 4);
            candidate->parameters.resize(inputs);
            std::memcpy(candidate->parameters.data(), h + 32 + p.usages * 4, inputs * 4);
            candidate->code.resize(p.bytes / 4);
            std::memcpy(candidate->code.data(), data.data() + p.start, p.bytes);
            candidate->code_hash = XXH3_64bits(candidate->code.data(), p.bytes);
            std::vector<u32> signature(candidate->regs.begin() + 2, candidate->regs.end());
            signature.insert(signature.end(), candidate->usages.begin(), candidate->usages.end());
            signature.insert(signature.end(), candidate->parameters.begin(), candidate->parameters.end());
            signature.push_back(u32(candidate->code_hash));
            signature.push_back(u32(candidate->code_hash >> 32));
            candidate->signature = XXH3_64bits(signature.data(), signature.size() * 4);
        }
        return true;
    }
    return false;
}
std::optional<Candidate> Fragment(std::span<const u8> data, const Program& p,
                                  const std::unordered_map<u32, u32>& exports) {
    for (u32 count = 0; count <= 32; ++count) {
        const size_t size = 52 + p.usages * 4 + ((count * 2 + 3) & ~3u);
        if (size > p.start)
            continue;
        const auto* h = data.data() + p.start - size;
        if (U32(h) != size + 8 || U32(h + 4) != 0xffffffff || U32(h + 48) != count ||
            (U32(h + 32) & 63) != count || ((U32(h + 12) >> 1) & 31) > 16)
            continue;
        Candidate c;
        c.hash = p.hash;
        std::memcpy(c.regs.data(), h, 52);
        c.usages.resize(p.usages);
        std::memcpy(c.usages.data(), h + 52, p.usages * 4);
        for (u32 i = 0; i < count; ++i) {
            const u16 semantic = U16(h + 52 + p.usages * 4 + i * 2);
            const auto found = exports.find(semantic & 255);
            if ((semantic & 0xf700) || found == exports.end() || found->second >= 32)
                return {};
            c.parameters.push_back(found->second);
        }
        c.code.resize(p.bytes / 4);
        std::memcpy(c.code.data(), data.data() + p.start, p.bytes);
        c.code_hash = XXH3_64bits(c.code.data(), p.bytes);
        std::vector<u32> signature(c.regs.begin() + 2, c.regs.end());
        signature.insert(signature.end(), c.usages.begin(), c.usages.end());
        signature.insert(signature.end(), c.parameters.begin(), c.parameters.end());
        signature.push_back(u32(c.code_hash));
        signature.push_back(u32(c.code_hash >> 32));
        c.signature = XXH3_64bits(signature.data(), signature.size() * 4);
        return c;
    }
    return {};
}
std::vector<fs::path> Archives(const fs::path& root) {
    std::vector<fs::path> result;
#ifdef GOW3_UWP
    const auto visit = [&](auto&& self, const fs::path& folder) -> void {
        WIN32_FIND_DATAA item{};
        const auto handle = gow3_uwp_find((folder / "*").string().c_str(), &item);
        if (handle == INVALID_HANDLE_VALUE)
            return;
        do {
            if (!std::strcmp(item.cFileName, ".") || !std::strcmp(item.cFileName, ".."))
                continue;
            const auto path = folder / item.cFileName;
            if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                self(self, path);
            else if (_stricmp(path.extension().string().c_str(), ".wad") == 0)
                result.push_back(path);
        } while (gow3_uwp_find_next(handle, &item));
        FindClose(handle);
    };
    visit(visit, root / "EXEC/WAD/PS4_LE");
#else
    for (const auto& entry : fs::recursive_directory_iterator(root / "EXEC/WAD/PS4_LE"))
        if (entry.is_regular_file() && entry.path().extension() == ".WAD")
            result.push_back(entry.path());
#endif
    std::sort(result.begin(), result.end());
    return result;
}
FILE* OpenDump(const fs::path& path) {
#ifdef GOW3_UWP
    return gow3_uwp_fopen(path.string().c_str(), "rb");
#else
    return std::fopen(path.string().c_str(), "rb");
#endif
}
int Seek(FILE* file, s64 offset, int origin) {
#ifdef _WIN32
    return _fseeki64(file, offset, origin);
#else
    return fseeko(file, offset, origin);
#endif
}
s64 Tell(FILE* file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return ftello(file);
#endif
}
void Scan(const fs::path& path, std::unordered_set<u64>& all,
          std::unordered_map<u64, Candidate>& candidates,
          std::unordered_map<u64, Candidate>& vertices) {
    FILE* file = OpenDump(path);
    if (!file)
        throw std::runtime_error("Cannot read shader archive: " + path.filename().string());
    const auto close = std::unique_ptr<FILE, decltype(&std::fclose)>(file, std::fclose);
    if (Seek(file, 0, SEEK_END))
        throw std::runtime_error("Cannot seek shader archive");
    const auto length = Tell(file);
    if (length < 0)
        throw std::runtime_error("Cannot size shader archive");
    u64 offset = 0;
    while (offset + 64 <= u64(length)) {
        std::array<u8, 64> header;
        if (Seek(file, offset, SEEK_SET) || std::fread(header.data(), 1, 64, file) != 64)
            throw std::runtime_error("Truncated WAD header");
        const u32 declared = U32(header.data() + 4);
        const bool count =
            U16(header.data()) == 0 && std::memcmp(header.data() + 8, "EntityCount\0", 12) == 0;
        const u64 size = count ? 0 : declared;
        if (offset + 64 + size > u64(length))
            throw std::runtime_error("WAD entry exceeds archive bounds");
        // Verified material resource tag in this title. Texture/model payloads
        // are skipped by seeking, keeping Xbox memory and USB I/O bounded.
        if (U16(header.data()) == 1 && U16(header.data() + 2) == 119) {
            if (size > 16 * 1024 * 1024)
                throw std::runtime_error("Material exceeds bounded precompile buffer");
            std::vector<u8> payload(size);
            if (std::fread(payload.data(), 1, size, file) != size)
                throw std::runtime_error("Truncated material");
            std::unordered_map<u32, u32> exports;
            std::optional<Candidate> vertex;
            for (size_t at = 0; at + 8 <= payload.size(); ++at) {
                auto program = ProgramAt(payload, at);
                if (!program)
                    continue;
                all.insert(program->hash);
                if (program->type == 1) {
                    exports.clear();
                    vertex.emplace();
                    if (!VertexExports(payload, *program, exports, &*vertex)) vertex.reset();
                } else if (program->type == 0) {
                    if (auto candidate = Fragment(payload, *program, exports)) {
                        candidates.try_emplace(candidate->hash, std::move(*candidate));
                        if (vertex) {
                            vertex->fragment = program->hash;
                            vertices.try_emplace(vertex->hash, std::move(*vertex));
                        }
                    }
                    vertex.reset();
                    exports.clear();
                }
            }
        }
        offset += 64 + ((size + 15) & ~u64(15));
    }
}
RuntimeInfo Runtime(const Candidate& c, u32 target) {
    RuntimeInfo r{};
    r.Initialize(HwStage::Fragment, SwStage::Fragment);
    AmdGpu::ShaderProgram pgm{};
    std::memcpy(&pgm.settings, c.regs.data() + 2, 8);
    r.props = {pgm.settings.fp_denorm_mode32,
               pgm.settings.fp_denorm_mode64,
               pgm.settings.fp_round_mode32,
               pgm.settings.fp_round_mode64,
               pgm.settings.num_user_regs,
               pgm.settings.vgpr_comp_cnt,
               pgm.NumVgprs()};
    auto& f = r.hw.fs;
    f.en_flags = std::bit_cast<AmdGpu::PsInput>(c.regs[6]);
    f.addr_flags = std::bit_cast<AmdGpu::PsInput>(c.regs[7]);
    f.front_face_all_bits =
        std::bit_cast<AmdGpu::BarycentricControl>(c.regs[9]).front_face_all_bits;
    f.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(c.regs[4]);
    const auto depth = std::bit_cast<AmdGpu::DepthShaderControl>(c.regs[10]);
    f.mrtz_mask =
        depth.z_export_enable |
        ((depth.stencil_op_val_export_enable || depth.stencil_test_val_export_enable) << 1) |
        (depth.mask_export_enable << 2) | (depth.coverage_to_mask_enable << 3);
    f.num_inputs = c.parameters.size();
    for (u32 i = 0; i < c.parameters.size(); ++i)
        f.inputs[i] = {u8(c.parameters[i]), false, false, 0};
    if (target)
        for (u32 i = 0; i < MaxColorBuffers; ++i) {
            const auto format = (c.regs[5] >> (i * 4)) & 15;
            if (!format)
                continue;
            auto& b = f.color_buffers[i];
            b.data_format = AmdGpu::DataFormat::Format8_8_8_8;
            b.num_format = AmdGpu::NumberFormat::Unorm;
            b.export_format = static_cast<AmdGpu::ShaderExportFormat>(format);
            b.swizzle = {.r = AmdGpu::CompSwizzle::Red,
                         .g = AmdGpu::CompSwizzle::Green,
                         .b = AmdGpu::CompSwizzle::Blue,
                         .a = AmdGpu::CompSwizzle::Alpha};
            if (target == 2)
                std::swap(b.swizzle.r, b.swizzle.b);
        }
    return r;
}
u64 RuntimeKey(const RuntimeInfo& r, Backend::Bindings start = {}) {
    std::vector<u32> words;
    auto add = [&](auto value) { words.push_back(u32(value)); };
    add(r.hw_stage);
    add(r.sw_stage);
    add(r.props.fp_denorm_mode32);
    add(r.props.fp_denorm_mode16_64);
    add(r.props.fp_round_mode32);
    add(r.props.fp_round_mode16_64);
    add(r.props.num_user_data);
    add(r.props.num_input_vgprs);
    add(r.props.num_allocated_vgprs);
    if (r.hw_stage == HwStage::Vertex) {
        add(start.unified); add(start.buffer); add(start.user_data);
        const auto& v = r.hw.vs;
        add(v.num_outputs);
        for (const auto& output : v.outputs) for (const auto value : output) add(value);
        add(v.emulate_depth_negative_one_to_one); add(v.clip_disable);
        add(v.user_clip_plane_mask); add(v.motion_vectors);
        add(r.sw.vs.step_rate_0); add(r.sw.vs.step_rate_1);
        add(r.sw.vs.vertex_sgpr_offset); add(r.sw.vs.instance_sgpr_offset);
        add(r.sw.vs.tess_emulated_primitive);
        return XXH3_64bits(words.data(), words.size() * 4);
    }
    const auto& f = r.hw.fs;
    add(std::bit_cast<u32>(f.en_flags));
    add(std::bit_cast<u32>(f.addr_flags));
    add(f.num_inputs);
    add(f.front_face_all_bits);
    add(f.z_export_format);
    add(f.num_samples);
    add(f.mrtz_mask);
    add(f.dual_source_blending);
    add(f.clip_distance_emulation);
    add(f.motion_vectors);
    for (u32 i = 0; i < f.num_inputs; ++i) {
        const auto& p = f.inputs[i];
        add(p.param_index);
        add(p.is_default);
        add(p.is_flat);
        add(p.default_value);
    }
    for (const auto& b : f.color_buffers) {
        add(b.export_format);
        if (b.export_format == AmdGpu::ShaderExportFormat::Zero)
            continue;
        // On the supported profile data-format differences affect shader code
        // only through needs_unorm_fixup (rejected at lookup). Number class,
        // conversion, export format and swizzle fully determine these exports.
        add(AmdGpu::GetNumberClass(b.num_format));
        add(b.num_conversion);
        add(b.swizzle.r);
        add(b.swizzle.g);
        add(b.swizzle.b);
        add(b.swizzle.a);
    }
    return XXH3_64bits(words.data(), words.size() * 4);
}
fs::path CacheFile(const fs::path& folder, u64 hash, const RuntimeInfo& r) {
    return folder / fmt::format("{:016x}-{:016x}.gsp", hash, RuntimeKey(r));
}
struct Header {
    u32 magic = 0x31505347, version = 1;
    u64 hash = 0, code_hash = 0, runtime_key = 0;
    Profile profile{};
    u32 words = 0, mrt_mask = 0, flags = 0;
    u64 checksum = 0;
};
bool Read(const fs::path& folder, u64 hash, u64 code_hash, const RuntimeInfo& runtime,
          const Profile& profile, std::vector<u32>& spirv, Header& header) {
    std::ifstream file(CacheFile(folder, hash, runtime), std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const auto size = file.tellg();
    file.seekg(0);
    if (size < std::streamoff(sizeof(Header)) ||
        !file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        header.magic != 0x31505347 || header.version != 1 || header.hash != hash ||
        header.code_hash != code_hash || header.runtime_key != RuntimeKey(runtime) ||
        header.profile != profile || header.words < 5 || header.words > 4 * 1024 * 1024 ||
        size != std::streamoff(sizeof(Header) + header.words * 4))
        return false;
    std::vector<u32> checked(header.words);
    if (!file.read(reinterpret_cast<char*>(checked.data()), checked.size() * 4) ||
        checked[0] != 0x07230203 ||
        XXH3_64bits(checked.data(), checked.size() * 4) != header.checksum)
        return false;
    spirv = std::move(checked);
    return true;
}
void Write(const fs::path& folder, const Candidate& c, const RuntimeInfo& runtime,
           const Profile& profile, const Info& info, std::span<const u32> spirv) {
    Header header;
    header.hash = c.hash;
    header.code_hash = c.code_hash;
    header.runtime_key = RuntimeKey(runtime);
    header.profile = profile;
    header.words = spirv.size();
    header.mrt_mask = info.mrt_mask;
    header.flags =
        u32(info.has_bitwise_xor) | (u32(info.uses_dma) << 1) | (u32(info.has_discard) << 2);
    header.checksum = XXH3_64bits(spirv.data(), spirv.size_bytes());
    const auto path = CacheFile(folder, c.hash, runtime);
    auto temp = path;
    temp += ".tmp";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(spirv.data()), spirv.size_bytes());
    file.close();
    if (!file)
        throw std::runtime_error("Cannot save precompiled shader");
    std::error_code error;
    fs::remove(path, error);
    fs::rename(temp, path);
}
Summary PrepareVertices(const std::unordered_map<u64, Candidate>& vertices,
                        const std::unordered_map<u64, Candidate>& fragments,
                        const fs::path& cache, const Profile& profile, u64 parent_library,
                        const Update& update) {
    struct Job { const Candidate* shader; std::vector<Backend::Bindings> starts; u32 classes; };
    std::vector<Job> jobs;
    std::vector<std::array<u64, 3>> inventory;
    std::unordered_set<u64> distinct;
    Progress progress;
    progress.scanning = false;
    progress.vertex = true;
    Summary result;
    for (const auto& [hash, c] : vertices) {
        const auto paired = fragments.find(c.fragment);
        if (paired == fragments.end()) { ++result.skipped; continue; }
        auto starts = TemplateBindingVariants(cache, c.fragment, paired->second.code_hash, profile);
        if (starts.empty()) { ++result.skipped; continue; }
        const bool bones = std::ranges::any_of(c.parameters, [](u32 s) { return (s & 255) == 15; });
        const u32 classes = bones ? 3 : 1;
        progress.total += starts.size() * 2 * classes;
        inventory.push_back({hash, c.signature, c.fragment});
        distinct.insert(c.code_hash);
        jobs.push_back({&c, std::move(starts), classes});
    }
    result.supported = jobs.size();
    result.distinct_programs = distinct.size();
    progress.supported = jobs.size();
    if (update) update(progress);
    if (jobs.empty()) return result;
    std::sort(inventory.begin(), inventory.end());
    inventory.push_back({parent_library, 0, 0});
    const u64 library = XXH3_64bits(inventory.data(), inventory.size() * sizeof(inventory[0]));
    const auto directory = cache / "vertex";
    u32 previous_entries = 0;
    if (TemplatesReady(directory, profile, library, previous_entries) && previous_entries == progress.total) {
        result.prepared_entries = jobs.size();
        result.already_cached = previous_entries;
        progress.done = progress.total;
        if (update) update(progress);
        std::printf("Precompile reused: %u vertex identities, %u variants\n", result.prepared_entries, previous_entries);
        return result;
    }
    TemplateWriter writer(directory, profile, library);
    std::map<std::array<u64, 3>, TemplateLocation> compiled;
    Pools pools;
    for (const auto& job : jobs) {
        const auto& c = *job.shader;
        bool ready = false;
        for (const auto& start : job.starts) for (u32 depth = 0; depth < 2; ++depth)
            for (u32 bone_class = 0; bone_class < job.classes; ++bone_class) {
                try {
                    RuntimeInfo runtime{};
                    runtime.Initialize(HwStage::Vertex, SwStage::Vertex);
                    AmdGpu::ShaderProgram pgm{};
                    std::memcpy(&pgm.settings, c.regs.data() + 2, 8);
                    runtime.props = {pgm.settings.fp_denorm_mode32, pgm.settings.fp_denorm_mode64,
                        pgm.settings.fp_round_mode32, pgm.settings.fp_round_mode64,
                        pgm.settings.num_user_regs, pgm.settings.vgpr_comp_cnt, pgm.NumVgprs()};
                    runtime.hw.vs.emulate_depth_negative_one_to_one = depth;
                    if (runtime.props.num_user_data > 16) throw std::runtime_error("Invalid vertex register count");
                    const auto key = RuntimeKey(runtime, start);
                    const std::array<u64, 3> identity{c.signature, key, bone_class};
                    if (auto found = compiled.find(identity); found != compiled.end()) {
                        writer.Alias(found->second, c.hash);
                        ++result.already_cached;
                        ready = true;
                    } else {
                        Info info;
                        info.hw_stage = HwStage::Vertex;
                        info.sw_stage = SwStage::Vertex;
                        info.pgm_hash = c.hash;
                        info.pgm_base = 0;
                        AssetInputs inputs(c.usages, 0, true);
                        info.offline_memory = &inputs;
                        info.offline_descriptor_snapshot = true;
                        info.user_data = inputs.UserData().first(runtime.props.num_user_data);
                        auto fetch = AssetFetch(c.parameters, c.usages, bone_class != 0, bone_class == 2);
                        fetch.info = &info;
                        Gcn::ScopedFetchShaderSnapshot scope(fetch);
                        const auto ir = TranslateProgram(c.code, pools, info, runtime, profile);
                        auto binding = start;
                        const auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
                        const auto location = writer.Write(c.code_hash, key, info, spirv, &fetch);
                        writer.Alias(location, c.hash);
                        compiled.emplace(identity, location);
                        ++result.compiled;
                        ready = true;
                    }
                } catch (const std::exception& error) {
                    ++result.failed;
                    std::printf("Precompile vertex skipped %016llx: %s\n", c.hash, error.what());
                }
                pools.ReleaseContents();
                ++progress.done;
                progress.failed = result.failed;
                if (update) update(progress);
            }
        if (ready) ++result.prepared_entries;
    }
    writer.Finish();
    std::printf("Precompile vertices: %u identities, %u distinct programs, %u compiled, %u aliases, %u failed\n",
        result.prepared_entries, result.distinct_programs, result.compiled, result.already_cached, result.failed);
    return result;
}
} // namespace

std::filesystem::path CacheDirectory() {
    return Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "precompiled" / "CUSA01715";
}
Summary Prepare(const fs::path& dump, const fs::path& cache, const Profile& profile,
                const Update& update) {
    if (profile.needs_unorm_fixup)
        throw std::runtime_error("Target profile requires unsupported output fixup");
    Summary result;
    const auto files = Archives(dump);
    if (files.empty())
        throw std::runtime_error("No game shader archives found");
    std::unordered_set<u64> all;
    std::unordered_map<u64, Candidate> candidates;
    std::unordered_map<u64, Candidate> vertices;
    Progress progress;
    progress.total = files.size();
    for (const auto& path : files) {
        Scan(path, all, candidates, vertices);
        ++progress.done;
        progress.supported = candidates.size();
        progress.skipped = all.size() - candidates.size();
        if (update)
            update(progress);
    }
    result.archives = files.size();
    result.supported = candidates.size();
    result.skipped = all.size() - candidates.size();
    std::unordered_set<u64> distinct;
    fs::create_directories(cache);
    // Hash a deterministic inventory, including aliases and all compiler inputs.
    // A completed compatible library can be reused without reading gigabytes of
    // SPIR-V during preparation; individual records are checked on actual use.
    std::vector<std::array<u64, 2>> inventory;
    for (const auto& [hash, c] : candidates) {
        distinct.insert(c.code_hash);
        inventory.push_back({hash, c.signature});
    }
    std::sort(inventory.begin(), inventory.end());
    const u64 library = XXH3_64bits(inventory.data(), inventory.size() * sizeof(inventory[0]));
    const auto finish_vertices = [&] {
        const auto v = PrepareVertices(vertices, candidates, cache, profile, library, update);
        result.supported += v.supported;
        result.prepared_entries += v.prepared_entries;
        result.distinct_programs += v.distinct_programs;
        result.skipped = result.skipped - vertices.size() + v.skipped;
        result.compiled += v.compiled;
        result.already_cached += v.already_cached;
        result.failed += v.failed;
    };
    result.distinct_programs = distinct.size();
    progress.scanning = false;
    progress.done = 0;
    progress.total = candidates.size() * 3;
    u32 previous_entries = 0;
    if (TemplatesReady(cache, profile, library, previous_entries) && previous_entries == progress.total) {
        result.prepared_entries = candidates.size();
        result.already_cached = previous_entries;
        progress.done = progress.total;
        if (update) update(progress);
        std::printf("Precompile reused: %u shader identities, %u prepared variants\n",
                    result.prepared_entries, previous_entries);
        finish_vertices();
        return result;
    }
    TemplateWriter writer(cache, profile, library);
    std::map<std::array<u64, 2>, TemplateLocation> compiled;
    Pools pools;
    for (const auto& [hash, c] : candidates) {
        bool ready = false;
        for (u32 target = 0; target < 3; ++target) {
            try {
                auto runtime = Runtime(c, target);
                const auto key = RuntimeKey(runtime);
                const std::array<u64, 2> identity{c.signature, key};
                if (auto found = compiled.find(identity); found != compiled.end()) {
                    writer.Alias(found->second, hash);
                    ++result.already_cached;
                    ready = true;
                } else {
                    Info info;
                    info.hw_stage = HwStage::Fragment;
                    info.sw_stage = SwStage::Fragment;
                    info.pgm_hash = hash;
                    info.pgm_base = 0;
                    AssetInputs inputs(c.usages, 0, true);
                    info.offline_memory = &inputs;
                    info.user_data = inputs.UserData().first(runtime.props.num_user_data);
                    info.offline_descriptor_snapshot = true;
                    Gcn::FetchShaderSnapshot fetch{.info = &info};
                    Gcn::ScopedFetchShaderSnapshot scope(fetch);
                    auto ir = TranslateProgram(c.code, pools, info, runtime, profile);
                    if (!info.fmasks.empty())
                        throw std::runtime_error("Shader needs an unsupported FMASK variant");
                    Backend::Bindings binding{};
                    auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
                    const auto location = writer.Write(c.code_hash, key, info, spirv);
                    writer.Alias(location, hash);
                    compiled.emplace(identity, location);
                    ++result.compiled;
                    ready = true;
                }
            } catch (const std::exception& error) {
                ++result.failed;
                std::printf("Precompile skipped %016llx/%u: %s\n", hash, target, error.what());
            }
            pools.ReleaseContents();
            ++progress.done;
            progress.failed = result.failed;
            if (update)
                update(progress);
        }
        if (ready)
            ++result.prepared_entries;
    }
    writer.Finish();
    finish_vertices();
    std::printf("Precompile complete: %u shader entries, %u distinct programs, "
                "%u compiled, %u cached, %u skipped, %u failed\n",
                result.supported, result.distinct_programs, result.compiled, result.already_cached,
                result.skipped, result.failed);
    return result;
}
bool TryLoad(Info& info, const RuntimeInfo& runtime, std::span<const u32> code,
             const Profile& profile, std::vector<u32>& spirv, Backend::Bindings start) {
    return TryLoadAt(CacheDirectory(), info, runtime, code, profile, spirv, start);
}
bool TryLoadAt(const fs::path& cache, Info& info, const RuntimeInfo& runtime,
               std::span<const u32> code, const Profile& profile, std::vector<u32>& spirv,
               Backend::Bindings start, const Gcn::FetchShaderSnapshot* captured_fetch) {
    const bool vertex = runtime.hw_stage == HwStage::Vertex && runtime.sw_stage == SwStage::Vertex;
    if ((!vertex && (runtime.hw_stage != HwStage::Fragment || runtime.sw_stage != SwStage::Fragment ||
        runtime.hw.fs.num_inputs > 32)) || profile.needs_unorm_fixup)
        return false;
    Header header;
    try {
        const auto code_hash = XXH3_64bits(code.data(), code.size_bytes());
        if (vertex) return TryTemplate(cache / "vertex", RuntimeKey(runtime, start), code_hash,
                                      info, runtime, profile, spirv, captured_fetch);
        if (!Read(cache, info.pgm_hash, code_hash, runtime, profile, spirv, header))
            return TryTemplate(cache, RuntimeKey(runtime), code_hash, info, runtime, profile, spirv);
    } catch (const std::exception&) {
        return false;
    }
    info.mrt_mask = header.mrt_mask;
    info.has_bitwise_xor = header.flags & 1;
    info.uses_dma = header.flags & 2;
    info.has_discard = header.flags & 4;
    info.flattened_ud_buf.resize(16);
    return true;
}
} // namespace Gow3Precompile

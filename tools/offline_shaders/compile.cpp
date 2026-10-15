// SPDX-License-Identifier: GPL-2.0-or-later
// Native asset preparation and headless compiler verification. Recorded cache
// metadata is a test oracle, not an asset-derived specialization. No guest or GPU.
#include <filesystem>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <span>
#include <cstring>
#include <cerrno>
#include <fmt/format.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/amdgpu/regs_depth.h"
#include "gow3_precompile.h"

namespace fs = std::filesystem;
using namespace Shader;

extern "C" {
u32 runtime_disabled_optimizations = 0;
FILE* gow3_uwp_fopen(const char* path, const char* mode) {
    if (std::string_view(mode) == "rb") return _wfopen(fs::path(path).c_str(), L"rb");
    std::fputs("Unexpected runtime file access in offline compiler\n", stderr);
    errno = EPERM;
    return nullptr;
}
HANDLE gow3_uwp_find(const char* pattern, WIN32_FIND_DATAA* data) {
    return FindFirstFileExA(pattern, FindExInfoBasic, data, FindExSearchNameMatch, nullptr, 0);
}
BOOL gow3_uwp_find_next(HANDLE handle, WIN32_FIND_DATAA* data) { return FindNextFileA(handle, data); }
}

static std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size < 0 || size > 64 * 1024 * 1024) {
        throw std::runtime_error("Cannot read bounded input: " + path.string());
    }
    std::vector<u8> data(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), data.size())) {
        throw std::runtime_error("Incomplete input: " + path.string());
    }
    return data;
}

struct Reader {
    std::span<const u8> bytes;
    void Read(void* destination, size_t size) {
        if (size > bytes.size()) throw std::runtime_error("Truncated native metadata");
        if (destination) std::memcpy(destination, bytes.data(), size);
        bytes = bytes.subspan(size);
    }
    template<class T> T Value() { T value{}; Read(&value, sizeof(value)); return value; }
    template<class V> void Vector(V& values, size_t limit = 4096) {
        const auto count = Value<size_t>();
        if (count > limit || count > bytes.size() / sizeof(typename V::value_type)) {
            throw std::runtime_error("Invalid native metadata vector size");
        }
        values.resize(count);
        Read(values.data(), count * sizeof(typename V::value_type));
    }
};

struct Case {
    u64 permutation_hash{};
    size_t permutation{};
    StageSpecialization spec;
    InfoPersistent recorded;
    std::vector<u32> flat;
    u32 flat_size{};
    size_t walker_bytes{};
};

static Case ReadCase(const fs::path& path) {
    const auto data = ReadFile(path);
    Reader reader{data};
    if (reader.Value<u32>() != 7 || reader.Value<u32>() != 7) {
        throw std::runtime_error("Requires current version 7 metadata and unsalted shader version 7");
    }
    Case result;
    result.permutation_hash = reader.Value<u64>();
    result.permutation = reader.Value<size_t>();
    result.spec.start = reader.Value<Backend::Bindings>();
    result.spec.runtime_info = reader.Value<RuntimeInfo>();
    std::string bits;
    reader.Vector(bits, 128);
    if (bits.size() != 128 || bits.find_first_not_of("01") != std::string::npos) {
        throw std::runtime_error("Invalid specialization bitset");
    }
    result.spec.bitset = decltype(result.spec.bitset)(bits);
    const auto fetch_size = reader.Value<size_t>();
    if (fetch_size) {
        if (fetch_size != sizeof(Gcn::FetchShaderData)) throw std::runtime_error("Fetch layout changed");
        Gcn::FetchShaderData fetch;
        fetch.size = reader.Value<u32>();
        fetch.vertex_offset_sgpr = reader.Value<s8>();
        fetch.instance_offset_sgpr = reader.Value<s8>();
        reader.Vector(fetch.attributes, 64);
        result.spec.fetch_shader_data = std::move(fetch);
    }
    reader.Vector(result.spec.vs_attribs, 64);
    reader.Vector(result.spec.buffers, NUM_BUFFERS);
    reader.Vector(result.spec.images, NUM_IMAGES);
    reader.Vector(result.spec.fmasks, NUM_FMASKS);
    reader.Vector(result.spec.samplers, NUM_SAMPLERS);
    reader.Read(&result.recorded, sizeof(InfoPersistent));
    if (result.recorded.buffers.size() > NUM_BUFFERS || result.recorded.images.size() > NUM_IMAGES ||
        result.recorded.samplers.size() > NUM_SAMPLERS || result.recorded.fmasks.size() > NUM_FMASKS) {
        throw std::runtime_error("Invalid persistent resource list sizes");
    }
    reader.Vector(result.flat, 1024 * 1024);
    // Read only the layout/size. The saved function pointer and machine code
    // are deliberately discarded, never registered or called.
    const auto srt = reader.Value<PersistentSrtInfo>();
    result.flat_size = srt.flattened_bufsize_dw;
    result.walker_bytes = srt.walker_func_size;
    reader.Read(nullptr, result.walker_bytes);
    // Archive::Writer may overallocate when a fetch record reserves space.
    const bool zero_padding = reader.bytes.size() <= sizeof(PersistentSrtInfo) &&
        std::ranges::all_of(reader.bytes, [](u8 value) { return value == 0; });
    if (!zero_padding || result.flat.size() != result.flat_size || result.flat.size() < 16 ||
        result.spec.runtime_info.props.num_user_data > 16 ||
        result.spec.runtime_info.hw_stage != result.recorded.hw_stage ||
        result.spec.runtime_info.sw_stage != result.recorded.sw_stage) {
        throw std::runtime_error(fmt::format(
            "Native metadata layout/stage mismatch: tail={}, flat={}/{}, ud={}, stages={}/{}, {}/{}",
            reader.bytes.size(), result.flat.size(), result.flat_size,
            result.spec.runtime_info.props.num_user_data,
            static_cast<u32>(result.spec.runtime_info.hw_stage), static_cast<u32>(result.recorded.hw_stage),
            static_cast<u32>(result.spec.runtime_info.sw_stage), static_cast<u32>(result.recorded.sw_stage)));
    }
    return result;
}

static void Describe(const fs::path& path, const Case& c, bool detailed = false) {
    std::string input_map;
    if (c.recorded.hw_stage == HwStage::Fragment) {
        const auto& fragment = c.spec.runtime_info.hw.fs;
        if (fragment.num_inputs > 32) throw std::runtime_error("Invalid interpolation count");
        for (u32 i = 0; i < fragment.num_inputs; ++i) {
            const auto& input = fragment.inputs[i];
            if (i) input_map += ',';
            input_map += fmt::format("{}", u32(input.param_index) | (u32(input.is_default) << 8) |
                (u32(input.is_flat) << 9) | (u32(input.default_value) << 16));
        }
    }
    std::cout << fmt::format(
        "{{\"meta\":\"{}\",\"hash\":\"0x{:016x}\",\"permutation\":{},"
        "\"stage\":{},\"buffers\":{},\"images\":{},\"samplers\":{},"
        "\"fetch\":{},\"flat_words\":{},\"walker_bytes\":{},"
        "\"common\":[{},{},{},{},{},{},{}],\"input_map\":[{}],\"start\":[{},{},{}]}}\n",
        path.filename().string(), c.recorded.pgm_hash, c.permutation,
        static_cast<u32>(c.recorded.hw_stage), c.recorded.buffers.size(),
        c.recorded.images.size(), c.recorded.samplers.size(),
        c.spec.fetch_shader_data.has_value() ? "true" : "false", c.flat_size, c.walker_bytes,
        c.spec.runtime_info.props.num_user_data, c.spec.runtime_info.props.num_input_vgprs,
        c.spec.runtime_info.props.num_allocated_vgprs,
        static_cast<u32>(c.spec.runtime_info.props.fp_round_mode32),
        static_cast<u32>(c.spec.runtime_info.props.fp_round_mode16_64),
        static_cast<u32>(c.spec.runtime_info.props.fp_denorm_mode32),
        static_cast<u32>(c.spec.runtime_info.props.fp_denorm_mode16_64), input_map,
        c.spec.start.unified, c.spec.start.buffer, c.spec.start.user_data);
    if (detailed && c.recorded.hw_stage == HwStage::Fragment) {
        const auto& f = c.spec.runtime_info.hw.fs;
        std::cerr << fmt::format("Fragment flags: ena={}, addr={}, inputs={}, z={}, samples={}, mrtz={}, front={}, dual={}, clip={}, motion={}\n",
            std::bit_cast<u32>(f.en_flags), std::bit_cast<u32>(f.addr_flags), f.num_inputs,
            u32(f.z_export_format), f.num_samples, f.mrtz_mask, f.front_face_all_bits,
            f.dual_source_blending, f.clip_distance_emulation, f.motion_vectors);
        for (u32 i = 0; i < MaxColorBuffers; ++i) {
            const auto& b = f.color_buffers[i];
            std::cerr << fmt::format("CB{}: data={}, num={}, conversion={}, export={}, swizzle={}/{}/{}/{}\n",
                i, u32(b.data_format), u32(b.num_format), u32(b.num_conversion), u32(b.export_format),
                u32(b.swizzle.r), u32(b.swizzle.g), u32(b.swizzle.b), u32(b.swizzle.a));
        }
    }
}

static Case ReadAssetCase(const fs::path& path, std::string_view target) {
    if (target != "depth-only" && target != "rgba8" && target != "bgra8") {
        throw std::runtime_error("Unknown asset target variant");
    }
    const auto bytes = ReadFile(path);
    Reader reader{bytes};
    if (reader.Value<u32>() != 0x31504647) {
        throw std::runtime_error("Invalid GFP1 fragment asset recipe");
    }
    const auto version = reader.Value<u32>();
    if (version != 1 && version != 2) throw std::runtime_error("Unknown fragment recipe version");
    Case c;
    c.recorded.pgm_hash = reader.Value<u64>();
    const auto size = reader.Value<u32>();
    if (size > reader.bytes.size() || size < 52 || size > 1136) {
        throw std::runtime_error("Invalid fragment asset header length");
    }
    std::array<u32, 13> regs;
    reader.Read(regs.data(), sizeof(regs));
    if (regs[0] != size + 8 || regs[1] != 0xffffffff || regs[12] > 32) {
        throw std::runtime_error("Invalid fragment asset header");
    }
    // No resource table provider yet. Interpolation comes from the paired
    // vertex's asset export semantics, never from recorded shader metadata.
    if (size != 52 + ((regs[12] * 2 + 3) & ~3u) || (regs[8] & 63) != regs[12]) {
        throw std::runtime_error("Asset-only proof requires no usage slots and matching input semantics");
    }
    const auto* semantics = reader.bytes.data();
    for (u32 i = 0; i < regs[12]; ++i) {
        u16 semantic;
        std::memcpy(&semantic, semantics + i * 2, 2);
        if (semantic >> 8) throw std::runtime_error("Unresolved asset interpolation flags");
    }
    reader.Read(nullptr, size - 52);
    std::vector<u32> parameters;
    if (version == 2) {
        const auto count = reader.Value<u32>();
        if (count != regs[12]) throw std::runtime_error("Asset interpolation map length mismatch");
        for (u32 i = 0; i < count; ++i) {
            const auto parameter = reader.Value<u32>();
            if (parameter >= 32) throw std::runtime_error("Invalid asset interpolation parameter");
            parameters.push_back(parameter);
        }
    } else if (regs[12]) {
        throw std::runtime_error("Linked inputs require recipe version 2");
    }
    if (!reader.bytes.empty()) throw std::runtime_error("Unexpected trailing asset recipe data");
    c.recorded.hw_stage = HwStage::Fragment;
    c.recorded.sw_stage = SwStage::Fragment;
    c.flat.resize(16);
    c.flat_size = 16;
    auto& runtime = c.spec.runtime_info;
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    AmdGpu::ShaderProgram pgm{};
    std::memcpy(&pgm.settings, regs.data() + 2, 8);
    runtime.props.num_user_data = pgm.settings.num_user_regs;
    runtime.props.num_input_vgprs = pgm.settings.vgpr_comp_cnt;
    runtime.props.num_allocated_vgprs = pgm.NumVgprs();
    runtime.props.fp_round_mode32 = pgm.settings.fp_round_mode32;
    runtime.props.fp_round_mode16_64 = pgm.settings.fp_round_mode64;
    runtime.props.fp_denorm_mode32 = pgm.settings.fp_denorm_mode32;
    runtime.props.fp_denorm_mode16_64 = pgm.settings.fp_denorm_mode64;
    if (runtime.props.num_user_data > 16) throw std::runtime_error("Too many user registers");
    auto& fragment = runtime.hw.fs;
    fragment.num_inputs = parameters.size();
    for (u32 i = 0; i < parameters.size(); ++i) {
        fragment.inputs[i] = {.param_index = u8(parameters[i]), .is_default = false,
                             .is_flat = false, .default_value = 0};
    }
    fragment.en_flags = std::bit_cast<AmdGpu::PsInput>(regs[6]);
    fragment.addr_flags = std::bit_cast<AmdGpu::PsInput>(regs[7]);
    fragment.front_face_all_bits = std::bit_cast<AmdGpu::BarycentricControl>(regs[9]).front_face_all_bits;
    fragment.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(regs[4]);
    const auto depth = std::bit_cast<AmdGpu::DepthShaderControl>(regs[10]);
    const bool stencil = depth.stencil_op_val_export_enable || depth.stencil_test_val_export_enable;
    fragment.mrtz_mask = depth.z_export_enable | (u32(stencil) << 1) |
        (depth.mask_export_enable << 2) | (depth.coverage_to_mask_enable << 3);
    for (u32 i = 0; i < MaxColorBuffers; ++i) {
        auto& color = fragment.color_buffers[i];
        // Explicit target variant, not a claim about every attachment used by
        // this material. Other formats and stage linkage remain planner work.
        if (target == "depth-only") continue;
        const auto export_format = static_cast<AmdGpu::ShaderExportFormat>((regs[5] >> (i * 4)) & 15);
        if (u32(export_format) == 0) continue;
        color.data_format = AmdGpu::DataFormat::Format8_8_8_8;
        color.num_format = AmdGpu::NumberFormat::Unorm;
        color.export_format = export_format;
        color.swizzle = {.r = AmdGpu::CompSwizzle::Red, .g = AmdGpu::CompSwizzle::Green,
                         .b = AmdGpu::CompSwizzle::Blue, .a = AmdGpu::CompSwizzle::Alpha};
        if (target == "bgra8") std::swap(color.swizzle.r, color.swizzle.b);
    }
    return c;
}

static int Compile(const fs::path& metadata, const fs::path& profile_path,
                   const fs::path& code_path, const fs::path& reference_path,
                   const fs::path& output, std::string_view asset_target = {}) {
    const bool asset_only = !asset_target.empty();
    auto c = asset_only ? ReadAssetCase(metadata, asset_target) : ReadCase(metadata);
    Describe(metadata, c);
    if (c.recorded.hw_stage != HwStage::Fragment || c.spec.fetch_shader_data) {
        throw std::runtime_error("First replay proof supports fragment shaders only");
    }
    if (fs::exists(output) || !fs::is_directory(output.parent_path())) {
        throw std::runtime_error("Output must be new, in an existing directory");
    }
    const auto profile_bytes = ReadFile(profile_path);
    if (profile_bytes.size() != sizeof(Profile)) throw std::runtime_error("Profile layout mismatch");
    Profile profile;
    std::memcpy(&profile, profile_bytes.data(), sizeof(profile));
    const auto bytes = ReadFile(code_path);
    if (bytes.size() < 8 || bytes.size() % 4) throw std::runtime_error("Invalid extracted code length");
    std::vector<u32> code(bytes.size() / 4);
    std::memcpy(code.data(), bytes.data(), bytes.size());
    if (code.front() != 0xBEEB03FF) throw std::runtime_error("Missing original shader token");
    const auto reference_name = fmt::format("0x{:016x}_{}.spv", c.recorded.pgm_hash, c.permutation);
    if (!reference_path.empty() && reference_path.filename() != reference_name) {
        throw std::runtime_error("Reference identity mismatch");
    }
    // Fresh compiler Info: retain descriptors, not the old compiler's derived
    // resource lists, flags, program address, or walker.
    Info info;
    info.hw_stage = c.recorded.hw_stage;
    info.sw_stage = c.recorded.sw_stage;
    info.pgm_hash = c.recorded.pgm_hash;
    info.pgm_base = 0;
    info.user_data = std::span(c.flat).first(c.spec.runtime_info.props.num_user_data);
    info.flattened_ud_buf = c.flat;
    Gcn::FetchShaderSnapshot fetch{.info = &info};
    Gcn::ScopedFetchShaderSnapshot fetch_scope(fetch);
    Pools pools;
    auto runtime = c.spec.runtime_info;
    auto binding = c.spec.start;
    const auto ir = TranslateProgram(code, pools, info, runtime, profile);
    if (asset_only && (!info.buffers.empty() || !info.images.empty() || !info.samplers.empty() ||
                       !info.fmasks.empty() || info.ud_mask.mask)) {
        throw std::runtime_error("Asset-only shader needs resource or user-data specialization");
    }
    const auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
    const auto result = std::as_bytes(std::span(spirv));
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(result.data()), result.size());
    file.close();
    if (!file) throw std::runtime_error("Could not write fresh compilation output");
    if (reference_path.empty()) {
        std::cout << fmt::format("{{\"fresh_compile\":true,\"uses_recorded_specialization\":false,"
                                "\"has_reference\":false,\"output_bytes\":{}}}\n", result.size());
        return 0;
    }
    const auto reference = ReadFile(reference_path);
    const bool equal = result.size() == reference.size() &&
        std::memcmp(result.data(), reference.data(), result.size()) == 0;
    std::cout << fmt::format("{{\"fresh_compile\":true,\"uses_recorded_specialization\":{},\"byte_equal\":{},"
        "\"output_bytes\":{},\"reference_bytes\":{}}}\n",
        asset_only ? "false" : "true", equal ? "true" : "false", result.size(), reference.size());
    return equal ? 0 : 3;
}

int main(int argc, char** argv) {
    try {
        // Avoid any diagnostic dump writes outside this tool's explicit output.
        _putenv_s("GOW3_DUMP_SHADERS", "0");
        // Same default as the UWP host; this also affects image specialization.
        if (!std::getenv("GOW3_NO_MSAA")) _putenv_s("GOW3_NO_MSAA", "1");
        AmdGpu::no_msaa.store(std::string_view(std::getenv("GOW3_NO_MSAA")) != "0");
        if (argc == 5 && std::string_view(argv[1]) == "prepare-native") {
            const auto data = ReadFile(argv[3]);
            if (data.size() != sizeof(Profile))
                throw std::runtime_error("Profile size mismatch");
            Profile profile;
            std::memcpy(&profile, data.data(), sizeof(profile));
            Gow3Precompile::Prepare(argv[2], argv[4], profile, [](const auto& p) {
                if (p.done % 50 == 0 || p.done == p.total)
                    std::cout << (p.scanning ? "Scanning " : "Compiling ") << p.done << '/'
                              << p.total << '\n';
            });
            return 0;
        }
        if (argc == 7 && std::string_view(argv[1]) == "verify-precompiled") {
            const auto c = ReadCase(argv[3]);
            const auto profile_data = ReadFile(argv[4]), code_data = ReadFile(argv[5]);
            if (profile_data.size() != sizeof(Profile) || code_data.size() < 8 ||
                code_data.size() % 4)
                throw std::runtime_error("Invalid profile/code");
            Profile profile;
            std::memcpy(&profile, profile_data.data(), sizeof(profile));
            std::vector<u32> code(code_data.size() / 4);
            std::memcpy(code.data(), code_data.data(), code_data.size());
            Info info;
            info.hw_stage = c.recorded.hw_stage;
            info.sw_stage = c.recorded.sw_stage;
            info.pgm_hash = c.recorded.pgm_hash;
            std::vector<u32> spv;
            if (!Gow3Precompile::TryLoadAt(argv[2], info, c.spec.runtime_info, code, profile, spv))
                return 4;
            const auto reference = ReadFile(argv[6]);
            const bool equal = reference.size() == spv.size() * 4 &&
                               !std::memcmp(reference.data(), spv.data(), reference.size());
            // A ready entry must never hide a changed program, runtime variant or profile.
            auto misses = [&](const auto& changed_code, const auto& runtime, const auto& target) {
                Info fresh;
                fresh.pgm_hash = info.pgm_hash;
                std::vector<u32> output;
                if (Gow3Precompile::TryLoadAt(argv[2], fresh, runtime, changed_code, target,
                                              output) ||
                    !output.empty())
                    throw std::runtime_error("Precompile cache accepted mismatched inputs");
            };
            auto changed_code = code;
            changed_code.back() ^= 1;
            misses(changed_code, c.spec.runtime_info, profile);
            auto changed_runtime = c.spec.runtime_info;
            changed_runtime.hw.fs.num_samples = 2;
            misses(code, changed_runtime, profile);
            auto changed_profile = profile;
            changed_profile.needs_unorm_fixup = true;
            misses(code, c.spec.runtime_info, changed_profile);
            std::cout << (equal ? "cache hit, exact\n" : "cache hit, DIFFERENT\n");
            return equal ? 0 : 3;
        }
        if (argc == 3 && std::string_view(argv[1]) == "inspect") {
            Describe(argv[2], ReadCase(argv[2]), true);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "inspect-asset") {
            Describe(argv[2], ReadAssetCase(argv[2], "depth-only"), true);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "list") {
            for (const auto& file : fs::directory_iterator(argv[2])) {
                if (file.path().extension() == ".meta") Describe(file.path(), ReadCase(file.path()));
            }
            return 0;
        }
        if (argc == 7 && std::string_view(argv[1]) == "compile") {
            return Compile(argv[2], argv[3], argv[4], argv[5], argv[6]);
        }
        if (argc == 8 && std::string_view(argv[1]) == "compile-asset") {
            return Compile(argv[3], argv[4], argv[5], argv[6], argv[7], argv[2]);
        }
        if (argc == 7 && std::string_view(argv[1]) == "emit-asset") {
            return Compile(argv[3], argv[4], argv[5], {}, argv[6], argv[2]);
        }
        std::cerr << "Usage: gow3-offline-shader list CACHE\n"
                  << "       gow3-offline-shader prepare-native DUMP PROFILE CACHE\n"
                  << "       gow3-offline-shader verify-precompiled CACHE META PROFILE GCN REFERENCE\n"
                  << "       gow3-offline-shader compile META PROFILE GCN REFERENCE OUTPUT\n"
                  << "       gow3-offline-shader compile-asset TARGET PSH PROFILE GCN REFERENCE OUTPUT\n"
                  << "       gow3-offline-shader emit-asset TARGET PSH PROFILE GCN OUTPUT\n"
                  << "       TARGET: depth-only, rgba8, or bgra8\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Offline shader: " << error.what() << '\n';
        return 2;
    }
}

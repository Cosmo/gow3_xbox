// SPDX-License-Identifier: GPL-2.0-or-later
// Headless experimental replay. Native cache metadata is an oracle, not an
// asset-derived specialization. No guest process, Vulkan device, or cache writes.
#include <filesystem>
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

namespace fs = std::filesystem;
using namespace Shader;

extern "C" {
u32 runtime_disabled_optimizations = 0;
FILE* gow3_uwp_fopen(const char*, const char*) {
    std::fputs("Unexpected runtime file access in offline compiler\n", stderr);
    errno = EPERM;
    return nullptr;
}
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

static void Describe(const fs::path& path, const Case& c) {
    std::cout << fmt::format(
        "{{\"meta\":\"{}\",\"hash\":\"0x{:016x}\",\"permutation\":{},"
        "\"stage\":{},\"buffers\":{},\"images\":{},\"samplers\":{},"
        "\"fetch\":{},\"flat_words\":{},\"walker_bytes\":{}}}\n",
        path.filename().string(), c.recorded.pgm_hash, c.permutation,
        static_cast<u32>(c.recorded.hw_stage), c.recorded.buffers.size(),
        c.recorded.images.size(), c.recorded.samplers.size(),
        c.spec.fetch_shader_data.has_value() ? "true" : "false", c.flat_size, c.walker_bytes);
}

static int Compile(const fs::path& metadata, const fs::path& profile_path,
                   const fs::path& code_path, const fs::path& reference_path,
                   const fs::path& output) {
    auto c = ReadCase(metadata);
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
    if (reference_path.filename() != reference_name) throw std::runtime_error("Reference identity mismatch");
    const auto reference = ReadFile(reference_path);
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
    const auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
    const auto result = std::as_bytes(std::span(spirv));
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(result.data()), result.size());
    file.close();
    if (!file) throw std::runtime_error("Could not write fresh compilation output");
    const bool equal = result.size() == reference.size() &&
        std::memcmp(result.data(), reference.data(), result.size()) == 0;
    std::cout << fmt::format("{{\"fresh_compile\":true,\"byte_equal\":{},"
        "\"output_bytes\":{},\"reference_bytes\":{}}}\n",
        equal ? "true" : "false", result.size(), reference.size());
    return equal ? 0 : 3;
}

int main(int argc, char** argv) {
    try {
        // Avoid any diagnostic dump writes outside this tool's explicit output.
        _putenv_s("GOW3_DUMP_SHADERS", "0");
        // Same default as the UWP host; this also affects image specialization.
        if (!std::getenv("GOW3_NO_MSAA")) _putenv_s("GOW3_NO_MSAA", "1");
        AmdGpu::no_msaa.store(std::string_view(std::getenv("GOW3_NO_MSAA")) != "0");
        if (argc == 3 && std::string_view(argv[1]) == "list") {
            for (const auto& file : fs::directory_iterator(argv[2])) {
                if (file.path().extension() == ".meta") Describe(file.path(), ReadCase(file.path()));
            }
            return 0;
        }
        if (argc == 7 && std::string_view(argv[1]) == "compile") {
            return Compile(argv[2], argv[3], argv[4], argv[5], argv[6]);
        }
        std::cerr << "Usage: gow3-offline-shader list CACHE\n"
                  << "       gow3-offline-shader compile META PROFILE GCN REFERENCE OUTPUT\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Offline shader: " << error.what() << '\n';
        return 2;
    }
}

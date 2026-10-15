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
#include "gow3_shader_inputs.h"
#include "gow3_shader_template.h"

namespace fs = std::filesystem;
using namespace Shader;
int RunOfflineShaderChecks();

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
    std::shared_ptr<Gow3Precompile::AssetInputs> asset_inputs;
    std::optional<Gcn::FetchShaderSnapshot> asset_fetch;
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
    if(detailed) {
        std::cerr << fmt::format("State 0x{:016x}\n",c.recorded.pgm_hash);
        Info info;
        static_cast<InfoPersistent&>(info)=c.recorded;
        info.flattened_ud_buf=c.flat;
        info.pgm_base=0;
        for(size_t i=0;i<c.recorded.buffers.size();++i) {
            const auto& desc=c.recorded.buffers[i];
            const auto sharp=desc.GetSharp(info);
            std::cerr << fmt::format("Buffer{}: type={}, stride={}, format={}/{}, formatted={}, swizzle={}, records={}, offsets=",i,
                u32(desc.buffer_type),sharp.GetStride(),u32(sharp.GetDataFmt()),u32(sharp.GetNumberFmt()),desc.is_formatted,sharp.swizzle_enable,sharp.num_records);
            for(auto off:desc.sharp_fetch.offsets) std::cerr << off << ',';
            std::cerr << '\n';
        }
        for(size_t i=0;i<c.recorded.images.size();++i) {
            const auto& desc=c.recorded.images[i];
            const auto sharp=desc.GetSharp(info);
            std::cerr << fmt::format("Image{}: type={}, format={}/{}, array={}, depth={}, written={}, offsets=",i,
                u32(sharp.GetType()),u32(sharp.GetDataFmt()),u32(sharp.GetNumberFmt()),desc.is_array,desc.is_depth,desc.is_written);
            for(auto off:desc.sharp_fetch.offsets) std::cerr << off << ',';
            std::cerr << '\n';
        }
        for(size_t i=0;i<c.recorded.samplers.size();++i) {
            const auto sharp=c.recorded.samplers[i].GetSharp(info);
            std::cerr << fmt::format("Sampler{}: unnormalized={}, degamma={}, raw={:016x}/{:016x}, offset={}\n",i,u32(sharp.force_unnormalized),u32(sharp.force_degamma),sharp.raw0,sharp.raw1,c.recorded.samplers[i].sharp_fetch.offsets[0]);
        }
        if(c.recorded.hw_stage==HwStage::Vertex) {
            const auto& v=c.spec.runtime_info.hw.vs;
            const auto& s=c.spec.runtime_info.sw.vs;
            std::cerr << fmt::format("Vertex: outputs={}, emulate_depth={}, clip_disable={}, clip_planes={}, motion={}, steps={}/{}, offsets={}/{}\n",
                v.num_outputs,v.emulate_depth_negative_one_to_one,v.clip_disable,v.user_clip_plane_mask,v.motion_vectors,s.step_rate_0,s.step_rate_1,s.vertex_sgpr_offset,s.instance_sgpr_offset);
        }
        if(c.spec.fetch_shader_data) {
            const auto& fetch=*c.spec.fetch_shader_data;
            std::cerr << fmt::format("Fetch: size={}, vertex_offset={}, instance_offset={}\n",fetch.size,fetch.vertex_offset_sgpr,fetch.instance_offset_sgpr);
            for(const auto& a:fetch.attributes) std::cerr << fmt::format("Fetch attr: semantic={}, vgpr={}, elements={}, sgpr={}, dword={}, instance={}, offset={}\n",
                a.semantic,a.dest_vgpr,a.num_elements,a.sgpr_base,a.dword_offset,a.instance_data,a.inst_offset);
        }
        for(const auto& a:c.spec.vs_attribs) std::cerr << fmt::format("VS format: divisor={}, class={}, mapping={}/{}/{}/{}\n",
            a.divisor,u32(a.num_class),u32(a.dst_select.r),u32(a.dst_select.g),u32(a.dst_select.b),u32(a.dst_select.a));
    }
}

static Case ReadVertexAssetCase(Reader& reader,std::string_view target,bool parametric) {
    const bool skinned=target.ends_with("-skin");
    if(skinned) target.remove_suffix(5);
    if(target!="vertex"&&target!="vertex-depth") throw std::runtime_error("Unknown vertex target variant");
    const auto version=reader.Value<u32>();
    if(version!=1&&version!=2) throw std::runtime_error("Unknown vertex recipe version");
    Case c;
    c.recorded.pgm_hash=reader.Value<u64>();
    const auto size=reader.Value<u32>();
    if(size<32||size>1244||reader.bytes.size()!=size+(version==2?12:0)) throw std::runtime_error("Invalid vertex header length");
    std::array<u32,8> regs;
    reader.Read(regs.data(),32);
    const auto inputs=regs[7]&255,exports=(regs[7]>>8)&255;
    const auto semantic_bytes=inputs*4+((exports*2+3)&~3u);
    if(regs[0]!=size+8||regs[1]!=0xffffffff||regs[7]>>16||inputs>32||exports>32||size<32+semantic_bytes||(size-32-semantic_bytes)%4||regs[6])
        throw std::runtime_error("Invalid vertex register/semantic layout");
    const auto usages_count=(size-32-semantic_bytes)/4;
    std::vector<u32> usages(usages_count),semantics(inputs);
    reader.Read(usages.data(),usages_count*4);reader.Read(semantics.data(),inputs*4);
    reader.Read(nullptr,(exports*2+3)&~3u);
    if(version==2) c.spec.start=reader.Value<Backend::Bindings>();
    c.asset_inputs=std::make_shared<Gow3Precompile::AssetInputs>(usages,0,parametric);
    c.asset_fetch=Gow3Precompile::AssetFetch(semantics,usages,skinned);
    c.recorded.hw_stage=HwStage::Vertex;c.recorded.sw_stage=SwStage::Vertex;
    c.flat.resize(16);c.flat_size=16;
    auto& runtime=c.spec.runtime_info;
    runtime.Initialize(HwStage::Vertex,SwStage::Vertex);
    AmdGpu::ShaderProgram pgm{};
    std::memcpy(&pgm.settings,regs.data()+2,8);
    runtime.props={pgm.settings.fp_denorm_mode32,pgm.settings.fp_denorm_mode64,
        pgm.settings.fp_round_mode32,pgm.settings.fp_round_mode64,
        pgm.settings.num_user_regs,pgm.settings.vgpr_comp_cnt,pgm.NumVgprs()};
    if(runtime.props.num_user_data>16) throw std::runtime_error("Invalid vertex user data count");
    runtime.hw.vs.emulate_depth_negative_one_to_one=target=="vertex-depth";
    return c;
}

static Case ParseAssetCase(std::span<const u8> bytes, std::string_view target) {
    const bool parametric=target.ends_with("-template");
    if(parametric) target.remove_suffix(9);
    Reader reader{bytes};
    const auto magic=reader.Value<u32>();
    if(magic==0x31505647) return ReadVertexAssetCase(reader,target,parametric);
    u32 sampler_flags=0;
    if(target.ends_with("-sampler1")) {sampler_flags=1;target.remove_suffix(9);}
    else if(target.ends_with("-sampler2")) {sampler_flags=2;target.remove_suffix(9);}
    else if(target.ends_with("-sampler3")) {sampler_flags=3;target.remove_suffix(9);}
    if (target != "depth-only" && target != "rgba8" && target != "bgra8") {
        throw std::runtime_error("Unknown asset target variant");
    }
    if (magic != 0x31504647) {
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
    // Resource slots and interpolation come from the packed game header.
    const u32 semantic_bytes = (regs[12] * 2 + 3) & ~3u;
    if (size < 52 + semantic_bytes || (size-52-semantic_bytes)%4 || (regs[8] & 63) != regs[12]) {
        throw std::runtime_error("Asset header usage/semantic length mismatch");
    }
    const u32 usage_count = (size-52-semantic_bytes)/4;
    std::vector<u32> usages(usage_count);
    reader.Read(usages.data(),usages.size()*4);
    const auto* semantics = reader.bytes.data();
    for (u32 i = 0; i < regs[12]; ++i) {
        u16 semantic;
        std::memcpy(&semantic, semantics + i * 2, 2);
        // Bit 11 requests linear interpolation, already selected by ENA/ADDR.
        // Flat/default semantic modes still need an explicit linked variant.
        if (semantic & 0xf700) throw std::runtime_error("Unresolved asset interpolation flags");
    }
    reader.Read(nullptr, semantic_bytes);
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
    if(!usages.empty()||parametric) c.asset_inputs = std::make_shared<Gow3Precompile::AssetInputs>(usages,sampler_flags,parametric);
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

static Case ReadAssetCase(const fs::path& path, std::string_view target) {
    const auto bytes = ReadFile(path);
    return ParseAssetCase(bytes, target);
}

static std::vector<u32> CompileAsset(Case& c, std::span<const u32> code,
                                     const Profile& profile, Pools& pools, Info& info,
                                     Backend::Bindings& binding) {
    if (code.size() < 2 || code.front() != 0xbeeb03ff ||
        c.spec.runtime_info.props.num_user_data > 16)
        throw std::runtime_error("Invalid asset compiler input");
    info.hw_stage = c.recorded.hw_stage;
    info.sw_stage = c.recorded.sw_stage;
    info.pgm_hash = c.recorded.pgm_hash;
    info.pgm_base = 0;
    info.user_data = std::span(c.flat).first(c.spec.runtime_info.props.num_user_data);
    info.flattened_ud_buf = c.flat;
    if (c.asset_inputs) {
        info.offline_memory = c.asset_inputs.get();
        info.user_data = c.asset_inputs->UserData().first(c.spec.runtime_info.props.num_user_data);
        info.offline_descriptor_snapshot = true;
    }
    Gcn::FetchShaderSnapshot fetch{.info = &info};
    if (c.asset_fetch) {
        fetch = *c.asset_fetch;
        fetch.info = &info;
    }
    Gcn::ScopedFetchShaderSnapshot scope(fetch);
    auto runtime = c.spec.runtime_info;
    binding = c.spec.start;
    const auto ir = TranslateProgram(code, pools, info, runtime, profile);
    if (!c.asset_inputs && (!info.buffers.empty() || !info.images.empty() ||
                           !info.samplers.empty() || !info.fmasks.empty() || info.ud_mask.mask))
        throw std::runtime_error("Asset shader requires descriptor inputs");
    return Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
}

// Streaming asset-only batch. A single compiler pool is reset between programs;
// no recorded metadata, guest execution, GPU, or per-shader process is involved.
// Results are append-only and checkpointed so an interrupted batch is inspectable.
static int CompileBatch(std::string_view target, const fs::path& input,
                        const fs::path& profile_path, const fs::path& directory) {
    if (fs::exists(directory)) throw std::runtime_error("Batch output must be new");
    const auto bytes = ReadFile(profile_path);
    if (bytes.size() != sizeof(Profile)) throw std::runtime_error("Profile layout mismatch");
    Profile profile;
    std::memcpy(&profile, bytes.data(), sizeof(profile));
    std::ifstream file(input, std::ios::binary);
    auto read = [&](void* dst, size_t size) {
        if (!file.read(static_cast<char*>(dst), size)) throw std::runtime_error("Truncated asset batch");
    };
    std::array<u32, 3> header;
    read(header.data(), sizeof(header));
    if (header[0] != 0x31424147 || header[1] != 1 || !header[2] || header[2] > 200000)
        throw std::runtime_error("Invalid asset batch header");
    fs::create_directories(directory);
    std::ofstream binary(directory / "templates.spvpack", std::ios::binary);
    std::ofstream index(directory / "index.jsonl");
    Pools pools;
    u32 failed = 0;
    for (u32 i = 0; i < header[2]; ++i) {
        u64 identity;
        std::array<u32, 2> sizes;
        read(&identity, sizeof(identity));
        read(sizes.data(), sizeof(sizes));
        if (sizes[0] < 20 || sizes[0] > 2048 || sizes[1] < 8 ||
            sizes[1] > 65536 || sizes[1] % 4)
            throw std::runtime_error("Invalid asset batch record sizes");
        std::vector<u8> recipe(sizes[0]);
        std::vector<u32> code(sizes[1] / 4);
        read(recipe.data(), recipe.size());
        read(code.data(), sizes[1]);
        // The pending record identifies a hard compiler failure too.
        if (i % 25 == 0) {
            binary.flush();
            index.flush();
            std::cout << fmt::format("Compiling {}/{} ({:016x}), failed={}\n", i, header[2], identity, failed) << std::flush;
        }
        try {
            auto c = ParseAssetCase(recipe, target);
            if (c.recorded.pgm_hash != identity) throw std::runtime_error("Batch identity mismatch");
            Info info;
            Backend::Bindings binding{};
            auto spirv = CompileAsset(c, code, profile, pools, info, binding);
            const u64 offset = u64(binary.tellp());
            binary.write(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4);
            index << fmt::format("{{\"index\":{},\"hash\":\"0x{:016x}\",\"outcome\":\"compiled\",\"offset\":{},\"words\":{},\"end_bindings\":[{},{},{}]}}\n",
                i, identity, offset, spirv.size(), binding.unified, binding.buffer, binding.user_data);
        } catch (const std::exception& error) {
            ++failed;
            index << fmt::format("{{\"index\":{},\"hash\":\"0x{:016x}\",\"outcome\":\"failed\"}}\n", i, identity);
            std::cerr << fmt::format("Batch skipped {:016x}: {}\n", identity, error.what());
        }
        pools.ReleaseContents();
        if (!index || !binary) throw std::runtime_error("Cannot write asset batch results");
    }
    if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing asset batch records");
    std::cout << fmt::format("Batch complete: {} compiled, {} failed\n", header[2] - failed, failed);
    return failed ? 3 : 0;
}

static int Compile(const fs::path& metadata, const fs::path& profile_path,
                   const fs::path& code_path, const fs::path& reference_path,
                   const fs::path& output, std::string_view asset_target = {}) {
    const bool asset_only = !asset_target.empty();
    auto c = asset_only ? ReadAssetCase(metadata, asset_target) : ReadCase(metadata);
    Describe(metadata, c);
    if ((!asset_only && c.recorded.hw_stage != HwStage::Fragment) || c.spec.fetch_shader_data) {
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
    if(c.asset_inputs) {
        info.offline_memory = c.asset_inputs.get();
        info.user_data = c.asset_inputs->UserData().first(c.spec.runtime_info.props.num_user_data);
        info.offline_descriptor_snapshot = true;
    }
    Gcn::FetchShaderSnapshot fetch{.info = &info};
    if(c.asset_fetch) {fetch=std::move(*c.asset_fetch);fetch.info=&info;}
    Gcn::ScopedFetchShaderSnapshot fetch_scope(fetch);
    Pools pools;
    auto runtime = c.spec.runtime_info;
    auto binding = c.spec.start;
    const auto ir = TranslateProgram(code, pools, info, runtime, profile);
    if (asset_only && !c.asset_inputs && (!info.buffers.empty() || !info.images.empty() || !info.samplers.empty() ||
                       !info.fmasks.empty() || info.ud_mask.mask)) {
        throw std::runtime_error("Asset-only shader needs resource or user-data specialization");
    }
    const auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
    std::cout << fmt::format("{{\"end_bindings\":[{},{},{}]}}\n",binding.unified,binding.buffer,binding.user_data);
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
        if(argc==2&&std::string_view(argv[1])=="selftest") return RunOfflineShaderChecks();
        if (argc == 6 && std::string_view(argv[1]) == "emit-batch")
            return CompileBatch(argv[2], argv[3], argv[4], argv[5]);
        if(argc==5&&std::string_view(argv[1])=="freeze-template") {
            const auto c=ReadCase(argv[3]);
            const auto input=ReadFile(argv[2]);
            if(input.size()%4||fs::exists(argv[4])) throw std::runtime_error("Invalid template or existing output");
            std::vector<u32> words(input.size()/4);
            std::memcpy(words.data(),input.data(),input.size());
            Info info;
            static_cast<InfoPersistent&>(info)=c.recorded;
            info.flattened_ud_buf=c.flat;
            info.user_data=std::span(c.flat).first(c.spec.runtime_info.props.num_user_data);
            info.pgm_base=0;
            Gcn::FetchShaderSnapshot fetch{.info=&info,.fetch=c.spec.fetch_shader_data};
            if(fetch.fetch) {
                if(c.spec.vs_attribs.size()!=fetch.fetch->attributes.size()) throw std::runtime_error("Fetch specialization count mismatch");
                for(const auto& a:c.spec.vs_attribs) {
                    auto buffer=AmdGpu::Buffer::Placeholder(1);
                    buffer.dst_sel_x=u32(a.dst_select.r);buffer.dst_sel_y=u32(a.dst_select.g);
                    buffer.dst_sel_z=u32(a.dst_select.b);buffer.dst_sel_w=u32(a.dst_select.a);
                    fetch.buffers.push_back(buffer);
                }
            }
            Gcn::ScopedFetchShaderSnapshot scope(fetch);
            std::vector<u32> frozen;
            if(!Gow3Precompile::FreezeTemplate(words,[&](u32 slot){return Gow3Precompile::DescriptorConstant(slot,info);},frozen))
                throw std::runtime_error("Could not resolve all template constants");
            std::ofstream output(argv[4],std::ios::binary);
            output.write(reinterpret_cast<const char*>(frozen.data()),frozen.size()*4);output.close();
            if(!output) throw std::runtime_error("Could not write frozen template");
            std::cout << "Template constants resolved without shader translation\n";
            return 0;
        }
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
        if (argc == 7 && (std::string_view(argv[1]) == "verify-precompiled" ||
                          std::string_view(argv[1]) == "emit-precompiled")) {
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
            info.user_data = std::span(c.flat).first(c.spec.runtime_info.props.num_user_data);
            info.flattened_ud_buf = c.flat;
            info.offline_descriptor_snapshot = true;
            std::vector<u32> spv;
            if (!Gow3Precompile::TryLoadAt(argv[2], info, c.spec.runtime_info, code, profile, spv))
                return 4;
            if (std::string_view(argv[1]) == "emit-precompiled") {
                if (fs::exists(argv[6])) throw std::runtime_error("Resolved output must be new");
                std::ofstream output(argv[6], std::ios::binary);
                output.write(reinterpret_cast<const char*>(spv.data()), spv.size() * 4);
                output.close();
                if (!output) throw std::runtime_error("Cannot write prepared shader resolution");
                std::cout << "Prepared template cache hit; no shader translation\n";
                return 0;
            }
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
        if (argc == 3 && (std::string_view(argv[1]) == "list" || std::string_view(argv[1]) == "list-states")) {
            for (const auto& file : fs::directory_iterator(argv[2])) {
                if (file.path().extension() == ".meta") Describe(file.path(), ReadCase(file.path()),std::string_view(argv[1])=="list-states");
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

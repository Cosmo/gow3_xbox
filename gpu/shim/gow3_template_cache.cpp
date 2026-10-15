// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_template_cache.h"
#include "gow3_shader_template.h"
#include "shader_recompiler/specialization.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <xxhash.h>

namespace Gow3Precompile {
namespace {
using namespace Shader;
namespace fs = std::filesystem;
constexpr u32 Version = 3;
struct LibraryHeader {
    u32 magic = 0x31495447, version = Version, info_size = sizeof(InfoPersistent), count = 0;
    Profile profile{};
    u64 library = 0, pack_bytes = 0, index_checksum = 0;
};
struct RecordHeader {
    u32 magic = 0x31525447, flags = 0, flat_words = 0, reads = 0, spirv_words = 0;
    InfoPersistent info;
};
struct Index {
    LibraryHeader header;
    std::unordered_multimap<u64, TemplateLocation> entries;
};
std::shared_ptr<Index> ReadIndex(const fs::path& cache, const Profile& profile) {
    const auto path = cache / "templates.gti";
    std::error_code error;
    const auto time = fs::last_write_time(path, error);
    if (error)
        return {};
    static std::mutex mutex;
    struct Cached { fs::file_time_type time; std::shared_ptr<Index> index; };
    static std::unordered_map<fs::path, Cached> cached;
    std::scoped_lock lock{mutex};
    if (auto found = cached.find(path); found != cached.end() && found->second.time == time &&
        found->second.index->header.profile == profile)
        return found->second.index;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    LibraryHeader header;
    file.seekg(0);
    if (size < std::streamoff(sizeof(header)) ||
        !file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        header.magic != 0x31495447 || header.version != Version ||
        header.info_size != sizeof(InfoPersistent) || header.profile != profile || !header.count ||
        header.count > 400000 ||
        size != std::streamoff(sizeof(header) + u64(header.count) * sizeof(TemplateLocation)) ||
        fs::file_size(cache / "templates.gtp", error) != header.pack_bytes || error)
        return {};
    std::vector<TemplateLocation> entries(header.count);
    if (!file.read(reinterpret_cast<char*>(entries.data()), entries.size() * sizeof(entries[0])) ||
        XXH3_64bits(entries.data(), entries.size() * sizeof(entries[0])) != header.index_checksum)
        return {};
    auto index = std::make_shared<Index>();
    index->header = header;
    for (const auto& e : entries) {
        if (e.offset > header.pack_bytes || e.bytes < sizeof(RecordHeader) ||
            e.bytes > 4 * 1024 * 1024 || e.bytes > header.pack_bytes - e.offset)
            return {};
        index->entries.emplace(e.hash, e);
    }
    cached[path] = {time, index};
    return index;
}
template <class T> void Append(std::vector<u8>& out, const T& data) {
    const auto* p = reinterpret_cast<const u8*>(&data);
    out.insert(out.end(), p, p + sizeof(data));
}
template <class T> void AppendSpan(std::vector<u8>& out, std::span<const T> data) {
    const auto* p = reinterpret_cast<const u8*>(data.data());
    if (!data.empty())
        out.insert(out.end(), p, p + data.size_bytes());
}
struct Reader {
    std::span<const u8> bytes;
    void Read(void* destination, size_t count) {
        if (count > bytes.size())
            throw std::runtime_error("Truncated prepared template");
        if (count)
            std::memcpy(destination, bytes.data(), count);
        bytes = bytes.subspan(count);
    }
    template <class T> T Value() {
        T value;
        Read(&value, sizeof(value));
        return value;
    }
};
// Match every descriptor property that was fixed during translation. The
// explicitly deferred fields are resolved in the already compiled SPIR-V.
bool Compatible(const Info& baseline, const Info& current, const RuntimeInfo& runtime,
                const Profile& profile, const Gcn::FetchShaderSnapshot& fetch) {
    if (!baseline.fmasks.empty() || (baseline.has_fetch_shader && !fetch.fetch))
        return false;
    StageSpecialization expected;
    {
        Gcn::ScopedFetchShaderSnapshot scope(fetch);
        expected = StageSpecialization(baseline, runtime, profile, {});
    }
    auto actual = StageSpecialization(current, runtime, profile, {});
    if (actual.vs_attribs.size() != expected.vs_attribs.size()) return false;
    for (size_t i = 0; i < actual.vs_attribs.size(); ++i)
        actual.vs_attribs[i].dst_select = expected.vs_attribs[i].dst_select;
    for (size_t i = 0; i < baseline.buffers.size(); ++i) {
        const auto& desc = baseline.buffers[i];
        if (desc.IsSpecial())
            continue;
        const auto sharp = desc.GetSharp(current);
        if (!sharp || !sharp.GetSize()) {
            if (desc.is_written)
                return false;
            actual.buffers[i] = expected.buffers[i];
        } else {
            if (sharp.GetStride() & 3) return false;
            actual.buffers[i].stride = expected.buffers[i].stride;
            if (!desc.is_written) actual.buffers[i].dst_select = expected.buffers[i].dst_select;
            if (desc.is_written && sharp.mtype != desc.GetSharp(baseline).mtype)
                return false;
        }
    }
    for (size_t i = 0; i < baseline.images.size(); ++i) {
        const auto& desc = baseline.images[i];
        if (!desc.GetSharp(current)) {
            if (desc.is_written)
                return false;
            actual.images[i] = expected.images[i];
        } else {
            // The sRGB choice only affects the deferred forced-gamma path for
            // sampled images; storage formats remain a fixed compatibility input.
            if (!desc.is_written)
                actual.images[i].is_srgb = expected.images[i].is_srgb;
        }
    }
    actual.samplers = expected.samplers;
    actual.bitset = expected.bitset;
    const bool equal = actual == expected;
    if (!equal && std::getenv("GOW3_PRECOMPILE_TRACE")) {
        if (actual.fetch_shader_data != expected.fetch_shader_data) {
            std::printf("Template fetch layout differs: actual=%zu expected=%zu\n",
                actual.fetch_shader_data ? actual.fetch_shader_data->attributes.size() : 0,
                expected.fetch_shader_data ? expected.fetch_shader_data->attributes.size() : 0);
            if (actual.fetch_shader_data && expected.fetch_shader_data)
                for (size_t i = 0; i < std::min(actual.fetch_shader_data->attributes.size(), expected.fetch_shader_data->attributes.size()); ++i) {
                    const auto& a = actual.fetch_shader_data->attributes[i];
                    const auto& e = expected.fetch_shader_data->attributes[i];
                    if (!(a == e)) std::printf("Fetch %zu: actual semantic/vgpr/elements/table/offset=%u/%u/%u/%u/%u expected=%u/%u/%u/%u/%u\n",
                        i, a.semantic, a.dest_vgpr, a.num_elements, a.sgpr_base, a.dword_offset,
                        e.semantic, e.dest_vgpr, e.num_elements, e.sgpr_base, e.dword_offset);
                }
        }
        for (size_t i = 0; i < actual.buffers.size(); ++i)
            if (!(actual.buffers[i] == expected.buffers[i]))
                std::printf("Template buffer %zu static format differs: actual=%u/%u swizzle=%u expected=%u/%u swizzle=%u\n",
                    i, actual.buffers[i].data_format, actual.buffers[i].num_format, actual.buffers[i].swizzle_enable,
                    expected.buffers[i].data_format, expected.buffers[i].num_format, expected.buffers[i].swizzle_enable);
        for (size_t i = 0; i < actual.vs_attribs.size(); ++i)
            if (!(actual.vs_attribs[i] == expected.vs_attribs[i]))
                std::printf("Template attribute %zu class differs: actual=%u expected=%u\n", i,
                    u32(actual.vs_attribs[i].num_class), u32(expected.vs_attribs[i].num_class));
    }
    return equal;
}
} // namespace

struct TemplateWriter::State {
    fs::path cache;
    LibraryHeader header;
    std::ofstream pack;
    std::vector<TemplateLocation> entries;
    bool finished = false;
};
TemplateWriter::TemplateWriter(const fs::path& cache, const Profile& profile, u64 library)
    : state(std::make_unique<State>()) {
    state->cache = cache;
    state->header.profile = profile;
    state->header.library = library;
    fs::create_directories(cache);
    state->pack.open(cache / "templates.gtp.tmp", std::ios::binary | std::ios::trunc);
    if (!state->pack)
        throw std::runtime_error("Cannot create template library");
}
TemplateWriter::~TemplateWriter() = default;
TemplateLocation TemplateWriter::Write(u64 code_hash, u64 runtime_key, const Info& info,
                                       std::span<const u32> spirv,
                                       const Gcn::FetchShaderSnapshot* fetch) {
    RecordHeader header;
    header.info = info;
    header.flags = u32(info.has_discard);
    header.flat_words = info.flattened_ud_buf.size();
    header.reads = info.offline_reads.size();
    header.spirv_words = spirv.size();
    std::vector<u8> data;
    Append(data, header);
    AppendSpan(data, std::span(info.flattened_ud_buf));
    for (const auto& read : info.offline_reads) {
        Append(data, u32(read.pointers.size()));
        Append(data, read.offset);
        Append(data, read.destination);
        AppendSpan(data, std::span(read.pointers));
    }
    if (info.has_fetch_shader) {
        if (!fetch || !fetch->fetch || fetch->fetch->attributes.size() != fetch->buffers.size())
            throw std::runtime_error("Prepared vertex is missing fetch layout");
        Append(data, u32(fetch->fetch->attributes.size()));
        Append(data, fetch->fetch->vertex_offset_sgpr);
        Append(data, fetch->fetch->instance_offset_sgpr);
        AppendSpan(data, std::span(fetch->fetch->attributes));
        AppendSpan(data, std::span(fetch->buffers));
    }
    AppendSpan(data, spirv);
    TemplateLocation entry{.hash = info.pgm_hash,
                           .code_hash = code_hash,
                           .runtime_key = runtime_key,
                           .offset = u64(state->pack.tellp()),
                           .checksum = XXH3_64bits(data.data(), data.size()),
                           .bytes = u32(data.size())};
    Backend::Bindings relative{};
    info.AddBindings(relative);
    if (relative.unified > 255 || relative.buffer > 255 || relative.user_data > 255)
        throw std::runtime_error("Prepared binding counts exceed format");
    entry.reserved = relative.unified | (relative.buffer << 8) | (relative.user_data << 16);
    state->pack.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!state->pack)
        throw std::runtime_error("Cannot write prepared template");
    return entry;
}
void TemplateWriter::Alias(TemplateLocation entry, u64 original_hash) {
    entry.hash = original_hash;
    state->entries.push_back(entry);
}
void TemplateWriter::Finish() {
    if (state->finished)
        throw std::runtime_error("Template library already completed");
    state->header.pack_bytes = u64(state->pack.tellp());
    state->pack.close();
    if (!state->pack)
        throw std::runtime_error("Cannot finish template library");
    state->header.count = state->entries.size();
    state->header.index_checksum =
        XXH3_64bits(state->entries.data(), state->entries.size() * sizeof(TemplateLocation));
    std::ofstream index(state->cache / "templates.gti.tmp", std::ios::binary | std::ios::trunc);
    index.write(reinterpret_cast<const char*>(&state->header), sizeof(state->header));
    index.write(reinterpret_cast<const char*>(state->entries.data()),
                state->entries.size() * sizeof(TemplateLocation));
    index.close();
    if (!index)
        throw std::runtime_error("Cannot finish template index");
    // The launcher prepares before gameplay. If interrupted between renames,
    // length/checksum validation makes the old index a harmless cache miss.
    std::error_code error;
    fs::remove(state->cache / "templates.gti", error);
    fs::remove(state->cache / "templates.gtp", error);
    fs::rename(state->cache / "templates.gtp.tmp", state->cache / "templates.gtp");
    fs::rename(state->cache / "templates.gti.tmp", state->cache / "templates.gti");
    state->finished = true;
}
bool TemplatesReady(const fs::path& cache, const Profile& profile, u64 library, u32& entries) {
    const auto index = ReadIndex(cache, profile);
    if (!index || index->header.library != library)
        return false;
    entries = index->header.count;
    return true;
}
std::vector<Backend::Bindings> TemplateBindingVariants(const fs::path& cache, u64 hash,
                                                       u64 code_hash, const Profile& profile) {
    std::vector<Backend::Bindings> result;
    const auto index = ReadIndex(cache, profile);
    if (!index) return result;
    const auto [first, last] = index->entries.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        const auto& e = it->second;
        if (e.code_hash != code_hash) continue;
        Backend::Bindings b{e.reserved & 255, (e.reserved >> 8) & 255, (e.reserved >> 16) & 255};
        if (std::ranges::find_if(result, [&](const auto& value) { return value == b; }) == result.end())
            result.push_back(b);
    }
    return result;
}
bool TryTemplate(const fs::path& cache, u64 runtime_key, u64 code_hash, Info& info,
                 const RuntimeInfo& runtime, const Profile& profile, std::vector<u32>& spirv,
                 const Gcn::FetchShaderSnapshot* captured_fetch) {
    try {
        const auto index = ReadIndex(cache, profile);
        if (!index)
            return false;
        const auto [first, last] = index->entries.equal_range(info.pgm_hash);
        for (auto it = first; it != last; ++it) {
            const auto& entry = it->second;
            if (entry.runtime_key != runtime_key || entry.code_hash != code_hash)
                continue;
            std::ifstream file(cache / "templates.gtp", std::ios::binary);
            file.seekg(entry.offset);
            std::vector<u8> data(entry.bytes);
            if (!file.read(reinterpret_cast<char*>(data.data()), data.size()) ||
                XXH3_64bits(data.data(), data.size()) != entry.checksum)
                continue;
            Reader reader{data};
            const auto header = reader.Value<RecordHeader>();
            if (header.magic != 0x31525447 || header.flat_words < 16 || header.flat_words > 16400 ||
                header.reads > 16384 || header.spirv_words < 5 || header.spirv_words > 1000000 ||
                header.info.hw_stage != runtime.hw_stage ||
                header.info.sw_stage != runtime.sw_stage ||
                header.info.buffers.size() > NUM_BUFFERS ||
                header.info.images.size() > NUM_IMAGES ||
                header.info.samplers.size() > NUM_SAMPLERS || !header.info.fmasks.empty())
                continue;
            Info baseline;
            static_cast<InfoPersistent&>(baseline) = header.info;
            baseline.pgm_base = 0;
            baseline.flattened_ud_buf.resize(header.flat_words);
            reader.Read(baseline.flattened_ud_buf.data(), header.flat_words * 4);
            SrtReadPlan plan;
            for (u32 i = 0; i < header.reads; ++i) {
                const auto count = reader.Value<u32>();
                if (!count || count > 16)
                    throw std::runtime_error("Invalid template pointer nesting");
                SrtRead read;
                read.offset = reader.Value<u32>();
                read.destination = reader.Value<u32>();
                read.pointers.resize(count);
                reader.Read(read.pointers.data(), count * 4);
                plan.push_back(std::move(read));
            }
            Gcn::FetchShaderSnapshot baseline_fetch{.info = &baseline};
            if (header.info.has_fetch_shader) {
                const u32 count = reader.Value<u32>();
                if (count > 32) throw std::runtime_error("Prepared vertex has too many attributes");
                baseline_fetch.fetch.emplace();
                baseline_fetch.fetch->vertex_offset_sgpr = reader.Value<s8>();
                baseline_fetch.fetch->instance_offset_sgpr = reader.Value<s8>();
                baseline_fetch.fetch->attributes.resize(count);
                baseline_fetch.buffers.resize(count);
                reader.Read(baseline_fetch.fetch->attributes.data(), count * sizeof(Gcn::VertexAttribute));
                reader.Read(baseline_fetch.buffers.data(), count * sizeof(AmdGpu::Buffer));
            }
            std::vector<u32> compiled(header.spirv_words);
            reader.Read(compiled.data(), compiled.size() * 4);
            if (!reader.bytes.empty())
                continue;
            Info prepared = info;
            static_cast<InfoPersistent&>(prepared) = header.info;
            prepared.pgm_hash = info.pgm_hash;
            prepared.has_discard = header.flags & 1;
            prepared.offline_memory = nullptr;
            prepared.offline_reads.clear();
            if (info.offline_descriptor_snapshot) {
                // Native verification supplies an explicit current-state snapshot.
                // Never follow pointer bytes captured in a previous process.
                if (info.flattened_ud_buf.size() != header.flat_words)
                    continue;
                prepared.srt_info.flattened_bufsize_dw = header.flat_words;
            } else {
                prepared.srt_info =
                    RegisterSrtReadPlan(plan, header.flat_words, runtime.props.num_user_data);
                prepared.RefreshFlatBuf();
            }
            Gcn::FetchShaderSnapshot explicit_fetch;
            std::optional<Gcn::ScopedFetchShaderSnapshot> current_scope;
            if (captured_fetch) {
                explicit_fetch = *captured_fetch;
                explicit_fetch.info = &prepared;
                current_scope.emplace(explicit_fetch);
            }
            if (!Compatible(baseline, prepared, runtime, profile, baseline_fetch))
                continue;
            std::vector<u32> resolved;
            if (!FreezeTemplate(
                    compiled, [&](u32 slot) { return DescriptorConstant(slot, prepared); },
                    resolved))
                continue;
            info = std::move(prepared);
            spirv = std::move(resolved);
            return true;
        }
    } catch (const std::exception&) {
        // An incomplete/incompatible/corrupt library never prevents gameplay.
    }
    return false;
}
} // namespace Gow3Precompile

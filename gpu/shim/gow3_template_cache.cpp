// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_template_cache.h"
#include "gow3_shader_template.h"
#include "shader_recompiler/specialization.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <xxhash.h>

namespace Gow3Precompile {
namespace {
using namespace Shader;
namespace fs = std::filesystem;
constexpr u32 Version = 1;
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
    static fs::path previous;
    static fs::file_time_type previous_time;
    static std::shared_ptr<Index> previous_index;
    std::scoped_lock lock{mutex};
    if (previous == path && previous_time == time && previous_index &&
        previous_index->header.profile == profile)
        return previous_index;
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
    previous = path;
    previous_time = time;
    previous_index = index;
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
                const Profile& profile) {
    if (!baseline.fmasks.empty() || baseline.has_fetch_shader)
        return false;
    const StageSpecialization expected(baseline, runtime, profile, {});
    auto actual = StageSpecialization(current, runtime, profile, {});
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
            actual.buffers[i].stride = expected.buffers[i].stride;
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
    return actual == expected;
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
                                       std::span<const u32> spirv) {
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
    AppendSpan(data, spirv);
    TemplateLocation entry{.hash = info.pgm_hash,
                           .code_hash = code_hash,
                           .runtime_key = runtime_key,
                           .offset = u64(state->pack.tellp()),
                           .checksum = XXH3_64bits(data.data(), data.size()),
                           .bytes = u32(data.size())};
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
bool TryTemplate(const fs::path& cache, u64 runtime_key, u64 code_hash, Info& info,
                 const RuntimeInfo& runtime, const Profile& profile, std::vector<u32>& spirv) {
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
            if (!Compatible(baseline, prepared, runtime, profile))
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

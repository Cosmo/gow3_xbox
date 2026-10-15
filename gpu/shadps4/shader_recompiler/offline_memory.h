// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include <vector>

namespace Shader {
// A bounded descriptor address space owned by an asset compiler. Addresses are
// synthetic identifiers, never pointers into a running or previous guest.
class OfflineMemory {
  public:
    virtual ~OfflineMemory() = default;
    virtual u32 Read32(u64 address) const = 0;
    virtual bool Parametric() const { return false; }
};

namespace OfflineSlot {
constexpr u32 Normalize = 0x1000;
constexpr u32 Gamma = 0x2000; // sampler * 64 + image
constexpr u32 Stride = 0x3000;
constexpr u32 Swizzle = 0x4000; // vertex attribute * 4 + component
constexpr u32 BufferPresent = 0x5000;
constexpr u32 ImagePresent = 0x6000;
}

struct SrtRead {
    // First offset is a user-data register pair; following offsets follow
    // pointers in descriptor tables. The leaf offset reads one data dword.
    std::vector<u32> pointers;
    u32 offset = 0;
    u32 destination = 0;
};
using SrtReadPlan = std::vector<SrtRead>;
struct PersistentSrtInfo;
// Register a validated asset-derived read layout against current guest tables.
// Called only for shaders actually used; compilation never dereferences a guest.
PersistentSrtInfo RegisterSrtReadPlan(const SrtReadPlan& plan, u32 flat_words, u32 user_words);
} // namespace Shader

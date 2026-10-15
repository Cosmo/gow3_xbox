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
};

struct SrtRead {
    // First offset is a user-data register pair; following offsets follow
    // pointers in descriptor tables. The leaf offset reads one data dword.
    std::vector<u32> pointers;
    u32 offset = 0;
    u32 destination = 0;
};
using SrtReadPlan = std::vector<SrtRead>;
} // namespace Shader

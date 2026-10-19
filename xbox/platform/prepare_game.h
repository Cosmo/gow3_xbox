#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Makes the launcher's game files from the player's own dump, byte for byte what scripts/prepare.py,
// link_modules.py, content_profile.py and xbox/scripts/prepare-graphics.py write.
namespace Gow3Prepare {
using Bytes=std::vector<uint8_t>;
struct Dump { Bytes eboot,libc,fios,sfo; };
struct Files { Bytes boot,content,patches_native,patches_720p; };
// Empty when param.sfo names God of War III Remastered, else what is wrong, for the player.
std::string CheckTitle(const Bytes& sfo);
// Throws std::runtime_error with the reason when the dump or the patch file cannot be used.
Files Prepare(const Dump& dump,const std::string& patch_xml);
// Identifies the dump and patch file that prepared files were made from.
uint64_t Fingerprint(const Dump& dump,const std::string& patch_xml);
}

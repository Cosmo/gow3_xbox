// SPDX-License-Identifier: GPL-2.0-or-later
// Optional standalone verification tool linked to official SPIRV-Tools.
// This is never linked into the game or required by the Xbox launcher.
#include <spirv-tools/libspirv.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: validate-batch PACK INDEX NEW-REPORT");
        std::ifstream pack(argv[1], std::ios::binary | std::ios::ate), index(argv[2]);
        const auto bytes = pack.tellg();
        if (!pack || !index || bytes < 0) throw std::runtime_error("Cannot read batch");
        std::ifstream existing(argv[3]);
        if (existing) throw std::runtime_error("Report must be new");
        std::ofstream report(argv[3]);
        const auto context = spvContextCreate(SPV_ENV_VULKAN_1_2);
        if (!context) throw std::runtime_error("Cannot initialize Vulkan 1.2 validator");
        auto number = [](const std::string& line, const char* field) {
            const auto at = line.find(field);
            if (at == std::string::npos) throw std::runtime_error("Invalid batch index");
            return std::strtoull(line.c_str() + at + std::char_traits<char>::length(field), nullptr, 10);
        };
        std::string line;
        std::vector<uint32_t> words;
        uint32_t count = 0, failed = 0;
        while (std::getline(index, line)) {
            if (line.find("\"outcome\":\"compiled\"") == std::string::npos) continue;
            const auto offset = number(line, "\"offset\":");
            const auto size = number(line, "\"words\":");
            if (size < 5 || size > 1000000 || offset > uint64_t(bytes) || size * 4 > uint64_t(bytes) - offset)
                throw std::runtime_error("Batch index exceeds SPIR-V container");
            words.resize(size);
            pack.seekg(offset);
            if (!pack.read(reinterpret_cast<char*>(words.data()), words.size() * 4))
                throw std::runtime_error("Truncated SPIR-V container");
            spv_diagnostic diagnostic = nullptr;
            const auto result = spvValidateBinary(context, words.data(), words.size(), &diagnostic);
            if (result != SPV_SUCCESS) {
                ++failed;
                report << "index=" << count << " " << line << '\n';
                if (diagnostic) report << diagnostic->error << '\n';
            }
            spvDiagnosticDestroy(diagnostic);
            ++count;
            if (count % 1000 == 0) std::cout << "Validated " << count << ", failed=" << failed << std::endl;
        }
        spvContextDestroy(context);
        report << "Validated " << count << ", failed=" << failed << '\n';
        std::cout << "Validated " << count << ", failed=" << failed << std::endl;
        return failed ? 3 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 2;
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// ampere-lower: offline Switch 2 (Ampere) shader program -> Maxwell instruction words.
//
// A cut-down build of Eden-NS2's tools/ampere-spirv (https://github.com/0x2593/Eden-NS2): the same
// capture and lowering headers, without the SPIR-V stage, so it compiles with a plain C++20 compiler
// (clang++ from Xcode's command line tools is enough). The nxnative shader chain only needs the lowered
// Maxwell program; its own translator turns that into Metal.
//
// usage: ampere-lower <program.bin> --stage N --header-bytes H --maxwell out.bin
// Output layout, as the emulator lays a lowered program out in memory: the 0x50-byte program header
// copied from the input, then the Maxwell words with a scheduling word (0) in front of every three
// instructions, a terminating zero, and the native address map (program_address_map.h).
// stdout: "OK stage=N ampere=<instructions in> maxwell=<instructions out>" on success, or one of
// READ_FAIL / FORMAT_FAIL / CAPTURE_FAIL / LOWER_FAIL (exit status 1 or 2), the words the corpus
// scripts look for.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {

constexpr size_t ProgramHeaderBytes = 0x50;

std::vector<char> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<char> data(size);
    file.seekg(0);
    file.read(data.data(), static_cast<std::streamsize>(size));
    return data;
}

int Usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s <program.bin> --stage N --header-bytes H --maxwell out.bin\n", argv0);
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return Usage(argv[0]);
    }
    const char* input = argv[1];
    const char* maxwell_output = nullptr;
    int stage_value = -1;
    long header_bytes = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--stage" && has_value) {
            stage_value = std::atoi(argv[++i]);
        } else if (arg == "--header-bytes" && has_value) {
            header_bytes = std::atol(argv[++i]);
        } else if (arg == "--maxwell" && has_value) {
            maxwell_output = argv[++i];
        } else if (arg == "-o" && has_value) {
            ++i;   // accepted and ignored: no SPIR-V stage here (keeps the ampere-spirv command line valid)
        } else if ((arg == "--workgroup" || arg == "--shared-mem" || arg == "--local-mem") && has_value) {
            ++i;   // SPIR-V-stage options, ignored
        } else {
            return Usage(argv[0]);
        }
    }
    if (stage_value < 0 || header_bytes < 0 || maxwell_output == nullptr) {
        return Usage(argv[0]);
    }
    const auto data = ReadFile(input);
    if (data.empty()) {
        std::fprintf(stderr, "READ_FAIL %s\n", input);
        return EXIT_FAILURE;
    }
    using Shader::Ampere::Instruction;
    if (static_cast<size_t>(header_bytes) > data.size() ||
        (data.size() - header_bytes) % sizeof(Instruction) != 0) {
        std::fprintf(stderr, "FORMAT_FAIL header %ld bytes, file %zu bytes\n", header_bytes, data.size());
        return EXIT_FAILURE;
    }
    std::vector<u64> code(ProgramHeaderBytes / sizeof(u64), 0);
    if (header_bytes >= static_cast<long>(ProgramHeaderBytes)) {
        std::memcpy(code.data(), data.data(), ProgramHeaderBytes);
    }
    const size_t count = (data.size() - header_bytes) / sizeof(Instruction);
    std::vector<Instruction> all(count);
    std::memcpy(all.data(), data.data() + header_bytes, count * sizeof(Instruction));

    // Stop where the emulator's capture stops (the program's EXIT, then any BRX tail blocks), so the
    // padding or unrelated bytes that follow a program inside a code section are never lowered.
    std::vector<Instruction> native;
    {
        using Capture = Shader::Ampere::ProgramCaptureState;
        Capture capture;
        for (u32 index = 0; index < all.size() && index < Capture::MaximumInstructions; ++index) {
            native.push_back(all[index]);
            const auto status = capture.Observe(all[index], index);
            if (status == Capture::Result::InvalidTarget) {
                std::printf("CAPTURE_FAIL branch outside the program at instruction %u\n", index);
                return 2;
            }
            if (status == Capture::Result::Complete) {
                break;
            }
        }
        // Constant buffers are not available offline, so BRX tables read as zero; the tail blocks they
        // would select are not extended (the same as ampere-spirv).
        Shader::Ampere::ResolveCaptureIndirectBranches(
            native, capture, static_cast<u32>(header_bytes),
            [&](u32 index) -> std::optional<Instruction> {
                if (index < all.size()) {
                    return all[index];
                }
                return std::nullopt;
            },
            [&](u32, u32) -> u32 { return 0; });
    }

    auto lowered = Shader::Ampere::LowerToMaxwell(native, {}, true);
    if (!lowered) {
        std::printf("LOWER_FAIL offset=%#x %.*s\n", lowered.error_offset,
                    static_cast<int>(lowered.error.size()), lowered.error.data());
        return 2;
    }
    for (const u64 instruction : lowered.instructions) {
        if ((code.size() * sizeof(u64)) % 32 == 0) {
            code.push_back(0);
        }
        code.push_back(instruction);
    }
    code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);

    std::ofstream out(maxwell_output, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "WRITE_FAIL %s\n", maxwell_output);
        return EXIT_FAILURE;
    }
    out.write(reinterpret_cast<const char*>(code.data()),
              static_cast<std::streamsize>(code.size() * sizeof(u64)));
    std::printf("OK stage=%d ampere=%zu maxwell=%zu\n", stage_value, count, lowered.instructions.size());
    return EXIT_SUCCESS;
}

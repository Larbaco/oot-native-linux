// Documented printing differences between our printer and LLVM 23 that are proven equivalent (see docs/m2-a64.md).
#pragma once
#include <cstdint>
#include <string>

namespace a64x {

// The rule's name when the pair (ours, llvm) for word w is a known, equivalent spelling; nullptr otherwise.
inline const char* explain(uint32_t w, const std::string& ours, const std::string& llvm) {
    (void)w, (void)ours, (void)llvm;
    return nullptr;
}

}  // namespace a64x

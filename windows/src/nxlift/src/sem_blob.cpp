// The semantics bitcode (src/sem, built by clang), embedded so nxlift is one executable
#include "nxlift/lift.hpp"

namespace {
alignas(16) const unsigned char blob[] = {
#embed "nxw_sem.bc"
};
}  // namespace

std::string_view nxlift::semantics_bitcode() { return {reinterpret_cast<const char*>(blob), sizeof blob}; }

// a64-dis: decodes words given in hex and prints the op, its fields and the assembly. Usage: a64-dis [--pc ADDR] word...
#include <cstdio>
#include <cstdlib>
#include <string>

#include "nxw/a64.hpp"

int main(int argc, char** argv) {
    uint64_t pc = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pc" && i + 1 < argc) {
            pc = std::strtoull(argv[++i], nullptr, 16);
            continue;
        }
        uint32_t w = static_cast<uint32_t>(std::strtoul(a.c_str(), nullptr, 16));
        auto op = nxw::a64::decode(w);
        std::printf("%08x  %-40s %s\n         ", w, nxw::a64::print(w, pc).c_str(), nxw::a64::op_name(op));
        for (auto& f : nxw::a64::op_fields(op)) std::printf(" %s=%u", f.name, nxw::a64::field(w, f));
        std::printf("\n");
        pc += 4;
    }
}

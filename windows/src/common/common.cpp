#include "nxw/common.hpp"

#include <fstream>

namespace nxw {

void fail(const std::string& what) { throw Error(what); }

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) fail("cannot open " + p.string());
    f.seekg(0, std::ios::end);
    std::vector<uint8_t> v(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    if (!v.empty() && !f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size()))) fail("cannot read " + p.string());
    return v;
}

void write_file(const std::filesystem::path& p, std::span<const uint8_t> data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) fail("cannot write " + p.string());
}

}  // namespace nxw

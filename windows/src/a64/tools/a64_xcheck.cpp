// a64-xcheck: compares the decoder and printer with LLVM's disassembler, word by word.
//   a64-xcheck [options] <words file>          raw little-endian words (a module's text from nso_text.py)
//   a64-xcheck [options] --random N            N random words
//   a64-xcheck [options] --patterns N          N random words inside each table encoding
//   a64-xcheck [options] --space MASK VALUE    every word with (w & MASK) == VALUE (at most 2^24)
// Options: --scratch DIR (where the temporary ELF goes; default: the words file's folder, else .), --seed S, --examples K,
//          --objdump PATH, --ops FILE (writes "op<TAB>extension<TAB>count" for the ops the input uses).
// One llvm-objdump process at a time, single-threaded. Exit code 1 when a difference is unexplained.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "nxw/a64.hpp"
#include "nxw/common.hpp"
#include "xcheck_rules.hpp"

#ifndef _WIN32   // the Mac build (docs/m4-swapin.md)
#define _pclose pclose
#endif

namespace fs = std::filesystem;
using namespace nxw;

namespace {

std::vector<uint8_t> elf_with_text(const std::vector<uint32_t>& words) {
    const char shstr[] = "\0.text\0.shstrtab";   // 17 bytes with the final NUL
    const size_t shstr_len = sizeof shstr, text_off = 0x40, code = words.size() * 4;
    const size_t shstr_off = text_off + code, sh_off = (shstr_off + shstr_len + 7) & ~size_t(7);
    std::vector<uint8_t> b(sh_off + 3 * 64);
    auto put = [&](size_t off, uint64_t v, int n) { std::memcpy(&b[off], &v, n); };
    std::memcpy(&b[0], "\x7f" "ELF\x02\x01\x01", 7);
    put(16, 1, 2); put(18, 183, 2); put(20, 1, 4); put(40, sh_off, 8);
    put(52, 64, 2); put(58, 64, 2); put(60, 3, 2); put(62, 2, 2);
    std::memcpy(&b[text_off], words.data(), code);
    std::memcpy(&b[shstr_off], shstr, shstr_len);
    size_t s = sh_off + 64;   // .text
    put(s, 1, 4); put(s + 4, 1, 4); put(s + 8, 6, 8); put(s + 24, text_off, 8); put(s + 32, code, 8); put(s + 48, 4, 8);
    s += 64;                  // .shstrtab
    put(s, 7, 4); put(s + 4, 3, 4); put(s + 24, shstr_off, 8); put(s + 32, shstr_len, 8); put(s + 48, 1, 8);
    return b;
}

// LLVM's line minus the parts that are not instruction text: the symbol annotation " <.text+0x..>" and "// =0x.." comments
std::string normalise(const char* line) {
    const char* t = std::strchr(line, '\t');
    std::string s = t ? t + 1 : line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (auto c = s.find(" //"); c != std::string::npos) s.erase(c);
    for (size_t p; (p = s.find(" <")) != std::string::npos;) {
        size_t e = s.find('>', p);
        if (e == std::string::npos) break;
        s.erase(p, e - p + 1);
    }
    std::replace(s.begin(), s.end(), '\t', ' ');
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct Diff {
    uint64_t count = 0;
    std::vector<std::string> examples;
};

}  // namespace

int main(int argc, char** argv) try {
    std::string input, objdump = "llvm-objdump", scratch, ops_out;
    uint64_t random_n = 0, patterns_n = 0, seed = 1, examples = 6;
    uint32_t space_mask = 0, space_value = 0;
    bool space = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after " + a); return argv[++i]; };
        if (a == "--random") random_n = std::stoull(next());
        else if (a == "--patterns") patterns_n = std::stoull(next());
        else if (a == "--space") { space_mask = std::stoul(next(), nullptr, 16); space_value = std::stoul(next(), nullptr, 16); space = true; }
        else if (a == "--seed") seed = std::stoull(next());
        else if (a == "--examples") examples = std::stoull(next());
        else if (a == "--objdump") objdump = next();
        else if (a == "--scratch") scratch = next();
        else if (a == "--ops") ops_out = next();
        else input = a;
    }
    std::vector<uint32_t> words;
    std::string label;
    if (!input.empty()) {
        auto bytes = read_file(input);
        words.resize(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), words.size() * 4);
        label = fs::path(input).stem().string();
        if (scratch.empty()) scratch = fs::path(input).parent_path().string();
    } else if (random_n) {
        std::mt19937_64 rng(seed);
        for (uint64_t i = 0; i < random_n; ++i) words.push_back(static_cast<uint32_t>(rng()));
        label = "random";
    } else if (patterns_n) {
        std::mt19937_64 rng(seed);
        for (unsigned op = 1; op < a64::op_count; ++op) {
            auto& e = a64::op_encoding(static_cast<a64::Op>(op));
            for (uint64_t i = 0; i < patterns_n; ++i) words.push_back((static_cast<uint32_t>(rng()) & ~e.mask) | e.value);
        }
        label = "patterns";
    } else if (space) {
        uint32_t free = ~space_mask;
        if (std::popcount(free) > 24) fail("--space: at most 24 free bits");
        for (uint32_t sub = free;; sub = (sub - 1) & free) {   // every subset of the free bits
            words.push_back(space_value | sub);
            if (!sub) break;
        }
        std::reverse(words.begin(), words.end());
        label = "space";
    } else {
        std::puts("usage: a64-xcheck [--scratch DIR] [--seed S] [--examples K] (<words file> | --random N | --patterns N | --space MASK VALUE)");
        return 2;
    }
    if (scratch.empty()) scratch = ".";

    // decoder speed on its own
    auto t0 = std::chrono::steady_clock::now();
    uint64_t sink = 0;
    for (uint32_t w : words) sink += static_cast<unsigned>(a64::decode(w));
    auto t1 = std::chrono::steady_clock::now();
    std::string tmp;
    for (size_t i = 0; i < words.size(); ++i) {
        tmp.clear();
        a64::print(tmp, words[i], i * 4);
        sink += tmp.size();
    }
    auto t2 = std::chrono::steady_clock::now();

    if (!ops_out.empty()) {
        std::vector<uint64_t> hist(a64::op_count);
        for (uint32_t w : words) ++hist[static_cast<unsigned>(a64::decode(w))];
        std::string tsv;
        unsigned used = 0;
        for (unsigned op = 0; op < a64::op_count; ++op)
            if (hist[op]) {
                used += op != 0;
                auto o = static_cast<a64::Op>(op);
                tsv += std::string(a64::op_name(o)) + '\t' + a64::op_encoding(o).ext + '\t' + std::to_string(hist[op]) + '\n';
            }
        write_file(ops_out, std::span(reinterpret_cast<const uint8_t*>(tsv.data()), tsv.size()));
        std::printf("%s: %u of %u encodings used\n", label.c_str(), used, a64::op_count - 1);
    }

    fs::path elf = fs::path(scratch) / ("xcheck_" + label + ".o");
    write_file(elf, elf_with_text(words));
    std::string cmd = "\"" + objdump + "\" -d -z --mattr=+v8.5a,+crypto \"" + elf.string() + "\"";
    FILE* p;
#ifdef _WIN32
    p = _popen(cmd.c_str(), "rb");
#else
    p = popen(cmd.c_str(), "r");
#endif
    if (!p) fail("cannot run " + objdump);

    std::map<std::string, Diff> unexplained;
    std::map<std::string, uint64_t> by_rule;
    uint64_t n = 0, exact = 0, unknown_both = 0, bad = 0;
    char line[1024];
    std::string ours;
    while (std::fgets(line, sizeof line, p)) {
        // instruction lines: "   <addr>: <8 hex digits> \t<mnemonic>\t<operands>"
        const char* colon = std::strchr(line, ':');
        if (!colon || !std::strchr(line, '\t') || line[0] != ' ') continue;
        const char* h = colon + 1;
        while (*h == ' ') ++h;
        uint32_t word = static_cast<uint32_t>(std::strtoul(h, nullptr, 16));
        if (n >= words.size() || word != words[n]) fail("llvm-objdump output out of step at word " + std::to_string(n));
        std::string ref = normalise(line);
        ours.clear();
        a64::print(ours, word, n * 4);
        if (ours == ref) {
            ++exact;
            if (ours == "<unknown>") ++unknown_both;
        } else if (const char* rule = a64x::explain(word, ours, ref)) {
            ++by_rule[rule];
        } else {
            ++bad;
            std::string key = ref == "<unknown>" ? std::string("<unknown> vs ") + a64::op_name(a64::decode(word))
                                                 : ref.substr(0, ref.find(' ')) + " (" + a64::op_name(a64::decode(word)) + ")";
            auto& d = unexplained[key];
            if (d.examples.size() < examples) {
                char b[16];
                std::snprintf(b, sizeof b, "%08x", word);
                d.examples.push_back(std::string(b) + "  ours: " + ours + "  |  llvm: " + ref);
            }
            ++d.count;
        }
        ++n;
    }
    int rc = _pclose(p);
    fs::remove(elf);
    if (rc != 0 || n != words.size()) fail("llvm-objdump failed or stopped early (" + std::to_string(n) + " of " + std::to_string(words.size()) + " words)");

    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("%s: %llu words, %llu identical (%llu undecodable in both), %llu by documented rules, %llu unexplained\n", label.c_str(),
                (unsigned long long)n, (unsigned long long)exact, (unsigned long long)unknown_both,
                (unsigned long long)(n - exact - bad), (unsigned long long)bad);
    for (auto& [rule, c] : by_rule) std::printf("  rule %-28s %llu\n", rule.c_str(), (unsigned long long)c);
    std::printf("  decode %.0f ms (%.1f ns/word), decode+print %.0f ms  [%llu]\n", ms(t0, t1), ms(t0, t1) * 1e6 / std::max<size_t>(1, words.size()),
                ms(t1, t2), (unsigned long long)(sink & 1));
    std::vector<std::pair<std::string, Diff>> sorted(unexplained.begin(), unexplained.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second.count > b.second.count; });
    for (auto& [k, d] : sorted) {
        std::printf("  UNEXPLAINED %-40s %llu\n", k.c_str(), (unsigned long long)d.count);
        for (auto& e : d.examples) std::printf("      %s\n", e.c_str());
    }
    return bad ? 1 : 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "a64-xcheck: %s\n", e.what());
    return 2;
}

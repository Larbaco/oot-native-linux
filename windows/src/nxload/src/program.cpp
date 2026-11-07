#include "nxload/program.hpp"

#include <cstdio>
#include <set>

namespace nxload {

using nxw::load_le;

namespace {

std::string hex(uint64_t v) {
    char s[24];
    std::snprintf(s, sizeof s, "%#llx", static_cast<unsigned long long>(v));
    return s;
}

uint64_t address_of(const Program& p, int m, uint32_t s) {
    const Symbol& d = p.modules[size_t(m)].symbols[s];
    return d.shndx == SHN_ABS ? d.value : p.bases[size_t(m)] + d.value;
}

void bind_symbols(Program& p, size_t i) {
    const Module& m = p.modules[i];
    auto& res = p.resolution[i];
    res.assign(m.symbols.size(), {});
    for (uint32_t s = 1; s < m.symbols.size(); s++) {
        const Symbol& sym = m.symbols[s];
        Resolution& r = res[s];
        if (sym.defined() && !sym.preemptible()) {
            r = {Bound::self, int(i), s, address_of(p, int(i), s)};
        } else if (sym.name.empty()) {
            continue;
        } else if (auto f = p.find(sym.name)) {
            r = {f->first == int(i) ? Bound::self : Bound::other, f->first, f->second, address_of(p, f->first, f->second)};
        } else if (sym.defined()) {
            r = {Bound::self, int(i), s, address_of(p, int(i), s)};   // a module outside the lookup scope (rtld)
        } else {
            r = {sym.bind == STB_WEAK ? Bound::weak_zero : Bound::unresolved, -1, 0, 0};
        }
    }
}

void relocate(Program& p, size_t i, bool write) {
    const Module& m = p.modules[i];
    RelocStats& st = p.stats[i];
    const uint64_t base = p.bases[i];
    std::vector<uint8_t>* mem = write ? &(p.memory[i] = m.image) : nullptr;
    auto put = [&](uint64_t off, uint64_t v) {
        if (mem) std::memcpy(mem->data() + off, &v, 8);
    };
    for (uint64_t a : m.relr) put(a, load_le<uint64_t>(m.image, size_t(a)) + base);
    st.relr = m.relr.size();
    std::set<uint32_t> reported;
    for (const auto* table : {&m.rela, &m.jmprel})
        for (const Reloc& r : *table) {
            st.applied[r.type]++;
            switch (r.type) {
            case R_AARCH64_NONE: break;
            case R_AARCH64_RELATIVE: put(r.offset, base + uint64_t(r.addend)); break;
            case R_AARCH64_ABS64:
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT: {   // S + A
                const Resolution& b = p.resolution[i][r.sym];
                switch (b.kind) {
                case Bound::self: st.sym_self++; break;
                case Bound::other: st.sym_other++; break;
                case Bound::weak_zero: st.sym_weak_zero++; break;
                case Bound::unresolved: st.sym_unresolved++; break;
                case Bound::none: break;   // symbol 0: S = 0
                }
                put(r.offset, b.address + uint64_t(r.addend));
                break;
            }
            default:
                if (reported.insert(r.type).second)
                    p.problems.push_back(m.name + ": unsupported relocation " + reloc_name(r.type) + " (first at " + hex(r.offset) + ")");
            }
        }
}

}  // namespace

std::string bound_name(Bound b) {
    switch (b) {
    case Bound::none: return "none";
    case Bound::self: return "self";
    case Bound::other: return "other";
    case Bound::weak_zero: return "weak_zero";
    case Bound::unresolved: return "unresolved";
    }
    return "?";
}

int Program::index(std::string_view name) const {
    for (size_t i = 0; i < modules.size(); i++)
        if (modules[i].name == name) return int(i);
    return -1;
}

int Program::module_at(uint64_t a) const {
    for (size_t i = 0; i < modules.size(); i++)
        if (a >= bases[i] && a - bases[i] < modules[i].image_size()) return int(i);
    return -1;
}

uint64_t Program::end() const {
    uint64_t e = 0;
    for (size_t i = 0; i < modules.size(); i++) e = std::max(e, bases[i] + modules[i].image_size());
    return e;
}

std::optional<std::pair<int, uint32_t>> Program::find(std::string_view name) const {
    for (size_t i = 0; i < modules.size(); i++) {
        if (!in_scope[i]) continue;
        const uint32_t s = modules[i].lookup(name);
        if (s && modules[i].symbols[s].visible()) return std::pair{int(i), s};
    }
    return std::nullopt;
}

uint64_t Program::read64(int m, uint64_t off) const {
    if (m < 0 || size_t(m) >= memory.size() || memory[size_t(m)].empty()) fail("read64: no relocated image");
    return load_le<uint64_t>(memory[size_t(m)], size_t(off));
}

std::vector<std::string> exefs_modules(const std::filesystem::path& dir, bool with_rtld) {
    std::vector<std::string> names = {"main"};
    for (int i = 0; i < 10; i++) names.push_back("subsdk" + std::to_string(i));
    names.push_back("sdk");
    if (with_rtld) names.push_back("rtld");
    std::erase_if(names, [&](const std::string& n) { return !std::filesystem::is_regular_file(dir / n); });
    return names;
}

Program link(std::vector<Module> modules, const LinkOptions& opt) {
    Program p;
    p.modules = std::move(modules);
    const size_t n = p.modules.size();
    uint64_t next = opt.base;
    for (const Module& m : p.modules) {
        auto it = opt.bases.find(m.name);
        const uint64_t b = it != opt.bases.end() ? it->second : next;
        if (b % page_size) fail("base " + hex(b) + " of " + m.name + " is not page-aligned");
        p.bases.push_back(b);
        p.in_scope.push_back(m.name != "rtld");
        next = align_up(b + m.image_size(), page_size);
    }
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (p.bases[i] < p.bases[j] + p.modules[j].image_size() && p.bases[j] < p.bases[i] + p.modules[i].image_size())
                fail(p.modules[i].name + " and " + p.modules[j].name + " overlap");

    p.resolution.resize(n);
    for (size_t i = 0; i < n; i++) bind_symbols(p, i);
    for (size_t i = 0; i < n; i++)
        for (uint32_t s = 1; s < p.modules[i].symbols.size(); s++)
            if (p.resolution[i][s].kind == Bound::unresolved) p.problems.push_back(p.modules[i].name + ": unresolved symbol " + p.modules[i].symbols[s].name);

    p.stats.resize(n);
    if (opt.relocate) p.memory.resize(n);
    for (size_t i = 0; i < n; i++) relocate(p, i, opt.relocate);
    return p;
}

Program load_program(const std::filesystem::path& exefs, bool with_rtld, const LoadOptions& lo, const LinkOptions& opt) {
    std::vector<Module> mods;
    for (const auto& n : exefs_modules(exefs, with_rtld)) mods.push_back(load_module_file(exefs / n, lo));
    if (mods.empty()) fail("no modules in " + exefs.string());
    return link(std::move(mods), opt);
}

}  // namespace nxload

#pragma once
#include "internal.hpp"

namespace nxanalyze {

struct LsdaCallSite {
    uint64_t start = 0, size = 0;   // module offsets
    uint64_t landing_pad = 0;       // module offset, 0 = none
    uint64_t action = 0;            // 0 = none, else 1 + offset into the action table
    int64_t first_filter = 0;
    std::string first_type;         // typeinfo symbol of the first catch clause, "" for catch (...)
    bool catch_all = false, cleanup = false;
    unsigned catches = 0, filters = 0;
};

struct Lsda {
    uint64_t lp_start = 0, ttype_base = 0, actions = 0;
    uint8_t ttype_enc = 0xff;
    std::vector<LsdaCallSite> sites;
};

Lsda parse_lsda(const ModCtx& c, uint32_t lsda, uint32_t fn_start);
std::string type_name_at(const ModCtx& c, uint64_t slot);   // the symbol a GOT-like slot is relocated against

}  // namespace nxanalyze

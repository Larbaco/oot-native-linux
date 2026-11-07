// .eh_frame_hdr and .eh_frame: CIEs, FDEs, augmentations and DW_EH_PE pointer encodings. Addresses are image offsets.
#pragma once
#include "nxload/module.hpp"

namespace nxload {

enum : uint8_t {
    DW_EH_PE_absptr = 0x00, DW_EH_PE_uleb128 = 0x01, DW_EH_PE_udata2 = 0x02, DW_EH_PE_udata4 = 0x03, DW_EH_PE_udata8 = 0x04,
    DW_EH_PE_sleb128 = 0x09, DW_EH_PE_sdata2 = 0x0a, DW_EH_PE_sdata4 = 0x0b, DW_EH_PE_sdata8 = 0x0c,
    DW_EH_PE_pcrel = 0x10, DW_EH_PE_textrel = 0x20, DW_EH_PE_datarel = 0x30, DW_EH_PE_funcrel = 0x40, DW_EH_PE_aligned = 0x50,
    DW_EH_PE_indirect = 0x80, DW_EH_PE_omit = 0xff,
};

// Reads one encoded pointer at p (advancing p). pcrel is relative to the field's own offset, datarel to datarel_base.
// The indirect bit is not followed (the caller gets the slot's address). Throws on textrel, funcrel and aligned.
uint64_t read_encoded(std::span<const uint8_t> b, size_t& p, uint8_t enc, uint64_t datarel_base = 0);

struct Cie {
    uint64_t offset = 0;   // of the length field
    uint8_t version = 0;
    std::string augmentation;
    uint64_t code_align = 0;
    int64_t data_align = 0;
    uint64_t ra_register = 0;
    uint8_t fde_enc = DW_EH_PE_absptr, lsda_enc = DW_EH_PE_omit, personality_enc = DW_EH_PE_omit;
    uint64_t personality = 0;   // the routine, or with DW_EH_PE_indirect the slot that holds it
    bool signal_frame = false, b_key = false, mte_tagged = false;
    uint64_t insns = 0, insns_size = 0;   // initial CFA instructions
};

struct Fde {
    uint64_t offset = 0;   // of the length field
    uint32_t cie = 0;      // index into EhFrame::cies
    uint64_t start = 0, size = 0;
    uint64_t lsda = 0;     // 0 when none
    uint64_t insns = 0, insns_size = 0;
};

struct EhFrameHdr {
    uint8_t version = 0, ptr_enc = 0, count_enc = 0, table_enc = 0;
    uint64_t eh_frame = 0, fde_count = 0;
    std::vector<std::pair<uint64_t, uint64_t>> table;   // (initial location, FDE offset), sorted by location
};

struct EhFrame {
    EhFrameHdr hdr;
    uint64_t begin = 0, end = 0;   // .eh_frame, end just past the zero terminator (or the limit)
    std::vector<Cie> cies;
    std::vector<Fde> fdes;         // in .eh_frame order
    bool hdr_matches = false;      // the search table names exactly the walked FDEs, start for start
    size_t lsda_count() const;
};

// hdr: the .eh_frame_hdr range; limit: where .eh_frame must end at the latest
EhFrame parse_eh_frame(std::span<const uint8_t> image, uint64_t hdr_start, uint64_t hdr_end, uint64_t limit);
EhFrame parse_eh_frame(const Module& m);   // located through MOD0, limited to the segment that holds .eh_frame

}  // namespace nxload

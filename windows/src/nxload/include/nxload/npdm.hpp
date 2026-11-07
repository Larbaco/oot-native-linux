// main.npdm: META, ACI0 and ACID, with service access and kernel capabilities (switchbrew's NPDM layout).
#pragma once
#include "nxload/module.hpp"

#include <bitset>

namespace nxload {

struct KernelCaps {
    std::vector<uint32_t> raw;
    std::map<int, int> kinds;     // descriptor count by kind (the number of low one bits)
    std::bitset<256> svcs;        // EnableSystemCalls
    bool has_thread_info = false;
    uint8_t lowest_priority = 0, highest_priority = 0, min_core = 0, max_core = 0;
    std::optional<uint32_t> program_type, kernel_major, kernel_minor, handle_table_size, misc_flags;
    int memory_maps = 0, io_maps = 0, interrupts = 0;
};

struct Service { std::string name; bool server = false; };

struct AccessControl {
    uint64_t program_id = 0;                  // ACI0
    uint64_t program_id_min = 0, program_id_max = 0;   // ACID
    uint32_t offset = 0, size = 0, magic_offset = 0, flags = 0;
    uint64_t fs_permissions = 0;
    std::vector<Service> services;
    KernelCaps kc;
};

struct Npdm {
    uint8_t flags = 0, main_thread_priority = 0, main_thread_core = 0;
    uint32_t system_resource_size = 0, version = 0, main_thread_stack_size = 0, reserved_10 = 0;
    std::string name, product_code;
    AccessControl aci0, acid;
    bool is_64bit() const { return flags & 1; }
    int address_space() const { return (flags >> 1) & 7; }
};

KernelCaps parse_kernel_caps(std::span<const uint8_t> b);
Npdm parse_npdm(std::span<const uint8_t> file);

}  // namespace nxload

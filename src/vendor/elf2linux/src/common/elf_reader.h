#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ElfInfo {
    bool valid = false;
    bool is_static = true;
    bool is_64bit = true;
    std::string interp;
    std::vector<std::string> needed;
};

ElfInfo read_elf_info(const std::string& path);

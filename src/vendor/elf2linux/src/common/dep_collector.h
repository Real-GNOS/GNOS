#pragma once

#include "elf_reader.h"
#include <string>

void collect_deps(const ElfInfo& elf, const std::string& elf_path,
                  const std::string& initramfs_root, bool do_strip);

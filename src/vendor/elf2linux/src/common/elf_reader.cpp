#include "elf_reader.h"
#include <cstdio>
#include <cstring>
#include <elf.h>

ElfInfo read_elf_info(const std::string& path) {
    ElfInfo info;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return info;

    unsigned char e_ident[EI_NIDENT];
    if (fread(e_ident, 1, EI_NIDENT, f) != EI_NIDENT) { fclose(f); return info; }
    if (memcmp(e_ident, ELFMAG, SELFMAG) != 0) { fclose(f); return info; }

    info.valid = true;
    bool is64 = (e_ident[EI_CLASS] == ELFCLASS64);
    info.is_64bit = is64;

    if (is64) {
        Elf64_Ehdr ehdr;
        rewind(f);
        if (fread(&ehdr, sizeof(ehdr), 1, f) != 1) { fclose(f); return info; }

        // read program headers for PT_INTERP
        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            Elf64_Phdr phdr;
            fseek(f, ehdr.e_phoff + i * ehdr.e_phentsize, SEEK_SET);
            if (fread(&phdr, sizeof(phdr), 1, f) != 1) break;
            if (phdr.p_type == PT_INTERP) {
                char buf[256];
                fseek(f, phdr.p_offset, SEEK_SET);
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                buf[n] = 0;
                info.interp = buf;
                info.is_static = false;
            }
        }

        // read .dynamic for NEEDED
        Elf64_Dyn dyn;
        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            Elf64_Phdr phdr;
            fseek(f, ehdr.e_phoff + i * ehdr.e_phentsize, SEEK_SET);
            if (fread(&phdr, sizeof(phdr), 1, f) != 1) break;
            if (phdr.p_type != PT_DYNAMIC) continue;

            fseek(f, phdr.p_offset, SEEK_SET);
            while (fread(&dyn, sizeof(dyn), 1, f) == 1) {
                if (dyn.d_un.d_val == DT_NULL) break;
                if (dyn.d_tag == DT_NEEDED) {
                    // we need the string table; go back and find it
                    // first pass: find DT_STRTAB
                    Elf64_Addr strtab_addr = 0;
                    off_t saved = ftell(f);
                    fseek(f, phdr.p_offset, SEEK_SET);
                    while (fread(&dyn, sizeof(dyn), 1, f) == 1) {
                        if (dyn.d_tag == DT_STRTAB) { strtab_addr = dyn.d_un.d_ptr; break; }
                        if (dyn.d_tag == DT_NULL) break;
                    }
                    fseek(f, saved, SEEK_SET);

                    // second pass for this NEEDED: find string
                    // Actually we need to do it properly - read all dyns first
                    break; // we'll do a cleaner pass below
                }
            }
            break; // only first PT_DYNAMIC
        }

        // clean approach: read all dynamic entries, find strtab, then resolve NEEDED
        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            Elf64_Phdr phdr;
            fseek(f, ehdr.e_phoff + i * ehdr.e_phentsize, SEEK_SET);
            if (fread(&phdr, sizeof(phdr), 1, f) != 1) break;
            if (phdr.p_type != PT_DYNAMIC) continue;

            Elf64_Addr strtab_addr = 0;
            std::vector<Elf64_Word> needed_idx;

            fseek(f, phdr.p_offset, SEEK_SET);
            while (fread(&dyn, sizeof(dyn), 1, f) == 1) {
                if (dyn.d_tag == DT_NULL) break;
                if (dyn.d_tag == DT_STRTAB) strtab_addr = dyn.d_un.d_ptr;
                if (dyn.d_tag == DT_NEEDED) needed_idx.push_back(dyn.d_un.d_val);
            }

            if (strtab_addr && !needed_idx.empty()) {
                // find file offset for strtab_addr via LOAD segments
                for (uint16_t j = 0; j < ehdr.e_phnum; j++) {
                    Elf64_Phdr load;
                    fseek(f, ehdr.e_phoff + j * ehdr.e_phentsize, SEEK_SET);
                    if (fread(&load, sizeof(load), 1, f) != 1) break;
                    if (load.p_type != PT_LOAD) continue;
                    if (strtab_addr >= load.p_vaddr &&
                        strtab_addr < load.p_vaddr + load.p_filesz) {
                        uint64_t offset = strtab_addr - load.p_vaddr + load.p_offset;
                        for (Elf64_Word idx : needed_idx) {
                            fseek(f, offset + idx, SEEK_SET);
                            char name[256];
                            size_t n = fread(name, 1, sizeof(name) - 1, f);
                            name[n] = 0;
                            info.needed.push_back(name);
                        }
                        break;
                    }
                }
            }
            break;
        }

    } else {
        // 32-bit ELF
        Elf32_Ehdr ehdr;
        rewind(f);
        if (fread(&ehdr, sizeof(ehdr), 1, f) != 1) { fclose(f); return info; }

        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            Elf32_Phdr phdr;
            fseek(f, ehdr.e_phoff + i * ehdr.e_phentsize, SEEK_SET);
            if (fread(&phdr, sizeof(phdr), 1, f) != 1) break;
            if (phdr.p_type == PT_INTERP) {
                char buf[256];
                fseek(f, phdr.p_offset, SEEK_SET);
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                buf[n] = 0;
                info.interp = buf;
                info.is_static = false;
            }
        }

        // 32-bit .dynamic
        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            Elf32_Phdr phdr;
            fseek(f, ehdr.e_phoff + i * ehdr.e_phentsize, SEEK_SET);
            if (fread(&phdr, sizeof(phdr), 1, f) != 1) break;
            if (phdr.p_type != PT_DYNAMIC) continue;

            Elf32_Addr strtab_addr = 0;
            std::vector<Elf32_Word> needed_idx;
            Elf32_Dyn dyn;

            fseek(f, phdr.p_offset, SEEK_SET);
            while (fread(&dyn, sizeof(dyn), 1, f) == 1) {
                if (dyn.d_tag == DT_NULL) break;
                if (dyn.d_tag == DT_STRTAB) strtab_addr = dyn.d_un.d_val;
                if (dyn.d_tag == DT_NEEDED) needed_idx.push_back(dyn.d_un.d_val);
            }

            if (strtab_addr && !needed_idx.empty()) {
                for (uint16_t j = 0; j < ehdr.e_phnum; j++) {
                    Elf32_Phdr load;
                    fseek(f, ehdr.e_phoff + j * ehdr.e_phentsize, SEEK_SET);
                    if (fread(&load, sizeof(load), 1, f) != 1) break;
                    if (load.p_type != PT_LOAD) continue;
                    if (strtab_addr >= load.p_vaddr &&
                        strtab_addr < load.p_vaddr + load.p_filesz) {
                        uint32_t offset = strtab_addr - load.p_vaddr + load.p_offset;
                        for (Elf32_Word idx : needed_idx) {
                            fseek(f, offset + idx, SEEK_SET);
                            char name[256];
                            size_t n = fread(name, 1, sizeof(name) - 1, f);
                            name[n] = 0;
                            info.needed.push_back(name);
                        }
                        break;
                    }
                }
            }
            break;
        }
    }

    fclose(f);
    return info;
}

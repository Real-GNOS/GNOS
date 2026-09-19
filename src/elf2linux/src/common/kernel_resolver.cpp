#include "kernel_resolver.h"
#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <regex>
#include <sys/stat.h>

KernelInfo find_host_kernel() {
    KernelInfo info;
    const char* boot_dirs[] = {"/boot", nullptr};

    struct KernelEntry {
        std::string path;
        std::string version;
    };

    std::vector<KernelEntry> vmlinuz_list;
    std::vector<KernelEntry> initrd_list;

    for (const char** d = boot_dirs; *d; ++d) {
        DIR* dp = opendir(*d);
        if (!dp) continue;

        struct dirent* entry;
        while ((entry = readdir(dp)) != nullptr) {
            std::string name = entry->d_name;

            // match vmlinuz-*
            if (name.find("vmlinuz-") == 0 || name.find("vmlinux-") == 0) {
                std::string ver = name.substr(name.find('-') + 1);
                vmlinuz_list.push_back({std::string(*d) + "/" + name, ver});
            }
            // match initrd.img-* or initramfs-*
            if (name.find("initrd.img-") == 0 || name.find("initramfs-") == 0) {
                std::string ver = name.substr(name.find('-') + 1);
                initrd_list.push_back({std::string(*d) + "/" + name, ver});
            }
        }
        closedir(dp);
    }

    if (vmlinuz_list.empty()) return info;

    // sort by version descending (simple string compare works for kernel versions)
    std::sort(vmlinuz_list.begin(), vmlinuz_list.end(),
              [](const KernelEntry& a, const KernelEntry& b) {
                  return a.version > b.version;
              });

    info.vmlinuz = vmlinuz_list.front().path;
    info.version = vmlinuz_list.front().version;

    // find matching initrd
    for (const auto& ir : initrd_list) {
        if (ir.version == info.version) {
            info.initrd = ir.path;
            break;
        }
    }

    // if no exact match, pick latest initrd
    if (info.initrd.empty() && !initrd_list.empty()) {
        std::sort(initrd_list.begin(), initrd_list.end(),
                  [](const KernelEntry& a, const KernelEntry& b) {
                      return a.version > b.version;
                  });
        info.initrd = initrd_list.front().path;
    }

    return info;
}

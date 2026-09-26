#pragma once

#include <string>

struct KernelInfo {
    std::string vmlinuz;
    std::string initrd;
    std::string version;
};

KernelInfo find_host_kernel();

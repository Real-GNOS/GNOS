#include "elf_reader.h"
#include "dep_collector.h"
#include "initramfs.h"
#include "kernel_resolver.h"
#include "pro_options.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* VERSION = "2.0.0";

static std::string join_path(const std::string& dir, const std::string& name) {
    if (!dir.empty() && dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
}

static void usage(const char* prog) {
    printf("Usage: %s [OPTIONS] <elf-file>\n", prog);
    printf("\n");
    printf("Build a bootable ISO from an ELF file + host Linux kernel.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -o, --output <path>    Output ISO path (default: <name>.iso)\n");
    printf("  -n, --name <name>      OS name (default: from ELF filename)\n");
    printf("  -k, --kernel <path>    Kernel path (default: auto-detect host kernel)\n");
    printf("  -r, --initrd <path>    Initrd path (default: auto-detect)\n");
    printf("  -t, --tmpdir <path>    Temp build directory (default: /tmp)\n");
    printf("  -s, --strip            Strip .so files (default: on)\n");
    printf("      --no-strip         Disable strip\n");
    printf("  -R, --no-root-check    Skip root privilege check\n");
    printf("      --iso-only         Only build ISO (skip initramfs if exists)\n");
    printf("  -v, --verbose          Verbose output\n");
    printf("  -q, --quiet            Quiet mode\n");
    printf("  -h, --help             Show this help\n");
    printf("  -V, --version          Show version\n");
    printf("\n");
    printf("Pro options (require --pro):\n");
    printf("      --pro               Enable advanced options\n");
    printf("      --kcmd <args>       Kernel command line (default: console=tty0 init=/init quiet)\n");
    printf("      --gzip-level <1-9>  Initramfs gzip compression level (default: 9)\n");
    printf("      --add-file <src:dst> Copy a host file into the initramfs (repeatable)\n");
    printf("      --initcmd <cmd>     What /init executes (default: exec /bin/os.elf)\n");
    printf("      --keep              Keep temporary build dirs/archive\n");
}

static std::string infer_name(const std::string& elf_path) {
    const char* base = basename((char*)elf_path.c_str());
    std::string name(base);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    // sanitize
    for (char& c : name) {
        if (!isalnum(c) && c != '-' && c != '_') c = '_';
    }
    return name.empty() ? "myos" : name;
}

static std::string find_xorriso() {
    const char* candidates[] = {"xorriso", "xorrisofs", nullptr};
    for (const char** c = candidates; *c; ++c) {
        std::string cmd = std::string("which ") + *c + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (p) {
            char buf[256];
            if (fgets(buf, sizeof(buf), p)) {
                size_t len = strlen(buf);
                while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
                pclose(p);
                return buf;
            }
            pclose(p);
        }
    }
    return "";
}

static std::string find_grub_mkrescue() {
    const char* candidates[] = {"grub-mkrescue", "grub2-mkrescue", nullptr};
    for (const char** c = candidates; *c; ++c) {
        std::string cmd = std::string("which ") + *c + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (p) {
            char buf[256];
            if (fgets(buf, sizeof(buf), p)) {
                size_t len = strlen(buf);
                while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
                pclose(p);
                return buf;
            }
            pclose(p);
        }
    }
    return "";
}

static bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

int main(int argc, char* argv[]) {
    // pre-scan for --help/--version/--no-root-check before root check
    bool skip_root_check = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-root-check") == 0 ||
            strcmp(argv[i], "-R") == 0) {
            skip_root_check = true;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ||
            strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            skip_root_check = true; // let getopt handle it
        }
    }

    // root check
    if (getuid() != 0 && !skip_root_check) {
        fprintf(stderr, "Error: this program requires root privileges.\n");
        fprintf(stderr, "  Run with: sudo %s ...\n", argv[0]);
        fprintf(stderr, "  Or bypass: %s --no-root-check ...\n", argv[0]);
        return 1;
    }

    static struct option long_opts[] = {
        {"output",       required_argument, nullptr, 'o'},
        {"name",         required_argument, nullptr, 'n'},
        {"kernel",       required_argument, nullptr, 'k'},
        {"initrd",       required_argument, nullptr, 'r'},
        {"tmpdir",       required_argument, nullptr, 't'},
        {"strip",        no_argument,       nullptr, 's'},
        {"no-strip",     no_argument,       nullptr, 'S'},
        {"iso-only",     no_argument,       nullptr, 'I'},
        {"no-root-check",no_argument,       nullptr, 'R'},
        {"verbose",      no_argument,       nullptr, 'v'},
        {"quiet",        no_argument,       nullptr, 'q'},
        {"help",         no_argument,       nullptr, 'h'},
        {"version",      no_argument,       nullptr, 'V'},
        {"pro",          no_argument,       nullptr, 1000},
        {"kcmd",         required_argument, nullptr, 1001},
        {"gzip-level",   required_argument, nullptr, 1002},
        {"add-file",     required_argument, nullptr, 1003},
        {"initcmd",      required_argument, nullptr, 1004},
        {"keep",         no_argument,       nullptr, 1005},
        {nullptr, 0, nullptr, 0}
    };

    std::string output, os_name, kernel_path, initrd_path, tmpdir = "/tmp";
    bool do_strip = true;
    bool verbose = false;
    bool quiet = false;
    bool iso_only = false;
    ProOptions pro;
    bool used_pro_opt = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "o:n:k:r:t:sSvqVRh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'o': output = optarg; break;
        case 'n': os_name = optarg; break;
        case 'k': kernel_path = optarg; break;
        case 'r': initrd_path = optarg; break;
        case 't': tmpdir = optarg; break;
        case 's': do_strip = true; break;
        case 'S': do_strip = false; break;
        case 'I': iso_only = true; break;
        case 'R': break; /* already handled above */
        case 'v': verbose = true; break;
        case 'q': quiet = true; break;
        case 'V': printf("elf2linux %s\n", VERSION); return 0;
        case 'h': usage(argv[0]); return 0;
        case 1000: pro.enabled = true; break;
        case 1001: pro.kcmd = optarg; used_pro_opt = true; break;
        case 1002: {
            char* end = nullptr;
            long lvl = strtol(optarg, &end, 10);
            if (!end || *end != '\0' || lvl < 1 || lvl > 9) {
                fprintf(stderr, "Error: --gzip-level must be 1..9\n");
                return 1;
            }
            pro.gzip_level = (int)lvl;
            used_pro_opt = true;
            break;
        }
        case 1003: {
            const char* colon = strchr(optarg, ':');
            if (!colon || colon == optarg || !colon[1]) {
                fprintf(stderr, "Error: --add-file needs <src>:<dst>\n");
                return 1;
            }
            pro.extra_files.emplace_back(std::string(optarg, colon - optarg),
                                         std::string(colon + 1));
            used_pro_opt = true;
            break;
        }
        case 1004: pro.init_cmd = optarg; used_pro_opt = true; break;
        case 1005: pro.keep_tmp = true; used_pro_opt = true; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (used_pro_opt && !pro.enabled) {
        fprintf(stderr, "Error: advanced options require --pro\n");
        return 1;
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: no ELF file specified\n");
        usage(argv[0]);
        return 1;
    }

    std::string elf_path = argv[optind];
    if (!file_exists(elf_path)) {
        fprintf(stderr, "Error: file not found: %s\n", elf_path.c_str());
        return 1;
    }

    if (os_name.empty()) os_name = infer_name(elf_path);
    if (output.empty()) output = os_name + ".iso";

    auto log = [&](const char* msg) {
        if (!quiet) printf("%s\n", msg);
    };
    auto logv = [&](const char* msg) {
        if (verbose) printf("[verbose] %s\n", msg);
    };

    // 1. Parse ELF
    if (!quiet) printf("[1/5] Parsing ELF: %s\n", elf_path.c_str());
    ElfInfo elf = read_elf_info(elf_path);
    if (!elf.valid) {
        fprintf(stderr, "Error: not a valid ELF file\n");
        return 1;
    }
    if (verbose) {
        printf("  arch: %s\n", elf.is_64bit ? "x86_64" : "i386");
        printf("  type: %s\n", elf.is_static ? "static" : "dynamic");
        if (!elf.is_static) printf("  interp: %s\n", elf.interp.c_str());
        if (!elf.needed.empty()) {
            printf("  needed:\n");
            for (const auto& n : elf.needed) printf("    %s\n", n.c_str());
        }
    }

    // 2. Find kernel
    if (!quiet) printf("[2/5] Locating host kernel...\n");
    KernelInfo kinfo;
    if (!kernel_path.empty()) {
        kinfo.vmlinuz = kernel_path;
    } else {
        kinfo = find_host_kernel();
    }
    if (kinfo.vmlinuz.empty()) {
        fprintf(stderr, "Error: no kernel found. Use -k to specify one.\n");
        return 1;
    }
    if (verbose) printf("  kernel: %s\n", kinfo.vmlinuz.c_str());

    if (!initrd_path.empty()) {
        kinfo.initrd = initrd_path;
    }
    if (!kinfo.initrd.empty() && verbose)
        printf("  initrd: %s\n", kinfo.initrd.c_str());

    // 3. Build initramfs
    if (!quiet) printf("[3/5] Building initramfs...\n");
    // make sure the temp dir exists
    std::string cmd = "mkdir -p \"" + tmpdir + "\"";
    system(cmd.c_str());
    struct stat tmpst;
    if (stat(tmpdir.c_str(), &tmpst) != 0 || !S_ISDIR(tmpst.st_mode)) {
        fprintf(stderr, "Error: tmpdir does not exist: %s\n", tmpdir.c_str());
        return 1;
    }
    std::string initramfs_dir = join_path(tmpdir, "elf2linux_initramfs_" + os_name);

    // clean and create
    cmd = "rm -rf \"" + initramfs_dir + "\" && mkdir -p \"" + initramfs_dir + "/bin\"";
    system(cmd.c_str());
    mkdir((initramfs_dir + "/proc").c_str(), 0755);
    mkdir((initramfs_dir + "/sys").c_str(), 0755);
    mkdir((initramfs_dir + "/dev").c_str(), 0755);
    mkdir((initramfs_dir + "/dev/pts").c_str(), 0755);
    mkdir((initramfs_dir + "/dev/shm").c_str(), 0755);
    mkdir((initramfs_dir + "/tmp").c_str(), 0755);
    mkdir((initramfs_dir + "/etc").c_str(), 0755);

    // write init script
    {
        std::string init_script =
            "#!/bin/sh\n"
            "mount -t proc     proc     /proc\n"
            "mount -t sysfs    sysfs    /sys\n"
            "mount -t devtmpfs devtmpfs /dev\n"
            "mkdir -p /dev/pts /dev/shm\n"
            "mount -t devpts devpts  /dev/pts 2>/dev/null\n"
            "mount -t tmpfs   tmpfs   /dev/shm\n"
            "mount -t tmpfs   tmpfs   /tmp\n"
            "export PATH=/bin:/usr/bin\n"
            "export HOME=/tmp\n"
            "export LD_LIBRARY_PATH=/lib:/lib64:/usr/lib:/usr/lib/x86_64-linux-gnu\n"
            "\n"
            "echo \"\"\n"
            "echo \"==============================\"\n"
            "echo \"  elf2linux - ELF Boot\"\n"
            "echo \"==============================\"\n"
            "echo \"\"\n"
            "\n"
            + pro.init_cmd + "\n";

        std::string init_path = initramfs_dir + "/init";
        FILE* f = fopen(init_path.c_str(), "w");
        if (f) {
            fprintf(f, "%s", init_script.c_str());
            fclose(f);
            chmod(init_path.c_str(), 0755);
        }
    }

    // collect deps
    collect_deps(elf, elf_path, initramfs_dir, do_strip);
    logv("deps collected");

    // pro: add extra files into the initramfs
    for (const auto& [src, dst] : pro.extra_files) {
        if (!file_exists(src)) {
            fprintf(stderr, "Error: --add-file source not found: %s\n", src.c_str());
            return 1;
        }
        std::string full = initramfs_dir + dst;
        size_t slash = full.rfind('/');
        if (slash != std::string::npos)
            mkdir(full.substr(0, slash).c_str(), 0755);
        cmd = "cp \"" + src + "\" \"" + full + "\"";
        system(cmd.c_str());
        logv(("added " + src + " -> " + dst).c_str());
    }

    // 4. Pack initramfs
    if (!quiet) printf("[4/5] Packing initramfs...\n");
    std::string initramfs_path = join_path(tmpdir, "elf2linux_" + os_name + ".cpio.gz");
    cmd = "cd \"" + initramfs_dir + "\" && find . -print0 | cpio -0 -o -H newc 2>/dev/null | gzip -" + std::to_string(pro.gzip_level) + " > \"" + initramfs_path + "\"";
    int rc = system(cmd.c_str());

    if (rc != 0 || !file_exists(initramfs_path)) {
        // fallback: try without -print0/-0
        cmd = "cd \"" + initramfs_dir + "\" && find . -print | cpio -o -H newc 2>/dev/null | gzip -" + std::to_string(pro.gzip_level) + " > \"" + initramfs_path + "\"";
        rc = system(cmd.c_str());
    }

    if (rc != 0 || !file_exists(initramfs_path)) {
        fprintf(stderr, "Error: cpio packing failed\n");
        return 1;
    }

    // guard: an empty or near-empty archive means packing silently failed
    // (e.g. cpio rejected the mode option); never emit a broken ISO.
    struct stat pst;
    if (stat(initramfs_path.c_str(), &pst) != 0 || pst.st_size < 100) {
        fprintf(stderr, "Error: initramfs archive is empty (%ld bytes); cpio packing failed\n",
                (long)(pst.st_size));
        return 1;
    }

    struct stat fst;
    stat(initramfs_path.c_str(), &fst);
    if (verbose) printf("  initramfs: %s (%.1f MB)\n", initramfs_path.c_str(),
                        fst.st_size / 1048576.0);

    // 5. Build ISO
    if (!quiet) printf("[5/5] Building ISO...\n");

    // create ISO layout
    std::string iso_dir = join_path(tmpdir, "elf2linux_iso_" + os_name);
    cmd = "rm -rf \"" + iso_dir + "\" && mkdir -p \"" + iso_dir + "/boot/grub\"";
    system(cmd.c_str());

    cmd = "cp \"" + kinfo.vmlinuz + "\" \"" + iso_dir + "/boot/bzImage\"";
    system(cmd.c_str());

    cmd = "cp \"" + initramfs_path + "\" \"" + iso_dir + "/boot/initramfs.cpio.gz\"";
    system(cmd.c_str());

    // grub.cfg
    std::string grub_cfg_path = iso_dir + "/boot/grub/grub.cfg";
    FILE* f = fopen(grub_cfg_path.c_str(), "w");
    if (f) {
        fprintf(f, "set timeout=0\nset default=0\n\n");
        fprintf(f, "menuentry \"%s\" {\n", os_name.c_str());
        fprintf(f, "    linux /boot/bzImage %s\n", pro.kcmd.c_str());
        fprintf(f, "    initrd /boot/initramfs.cpio.gz\n");
        fprintf(f, "}\n");
        fclose(f);
    }

    // try grub-mkrescue first, then xorriso
    std::string grub = find_grub_mkrescue();
    if (!grub.empty()) {
        if (verbose) printf("  using %s\n", grub.c_str());
        cmd = "\"" + grub + "\" -o \"" + output + "\" \"" + iso_dir + "\"";
        rc = system(cmd.c_str());
    } else {
        rc = 1;
    }

    if (rc != 0) {
        std::string xr = find_xorriso();
        if (!xr.empty()) {
            if (verbose) printf("  fallback: %s\n", xr.c_str());
            cmd = "\"" + xr + "\" -as mkisofs -iso-level 3 -full-iso9660-filenames "
                  "-volid \"" + os_name.substr(0, 32) + "\" "
                  "-output \"" + output + "\" \"" + iso_dir + "\"";
            rc = system(cmd.c_str());
        } else {
            fprintf(stderr, "Error: neither grub-mkrescue nor xorriso found\n");
            return 1;
        }
    }

    if (rc != 0 || !file_exists(output)) {
        fprintf(stderr, "Error: ISO creation failed\n");
        return 1;
    }

    stat(output.c_str(), &fst);
    printf("\n");
    if (!quiet) printf("Done! ISO: %s (%.1f MB)\n", output.c_str(), fst.st_size / 1048576.0);
    printf("Test: qemu-system-x86_64 -cdrom %s -m 512M\n", output.c_str());

    // cleanup
    if (pro.keep_tmp) {
        if (verbose) {
            printf("  keeping temp files:\n");
            printf("    %s\n    %s\n    %s\n", initramfs_dir.c_str(),
                   iso_dir.c_str(), initramfs_path.c_str());
        }
    } else {
        cmd = "rm -rf \"" + initramfs_dir + "\" \"" + iso_dir + "\" \"" + initramfs_path + "\"";
        system(cmd.c_str());
    }

    return 0;
}

#include "dep_collector.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <libgen.h>
#include <regex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

static void mkdir_p(const std::string& path) {
    std::string cmd = "mkdir -p \"" + path + "\"";
    system(cmd.c_str());
}

static void copy_file(const std::string& src, const std::string& dst) {
    mkdir_p(dst.substr(0, dst.rfind('/')));
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    out << in.rdbuf();
    chmod(dst.c_str(), 0755);
}

static void strip_file(const std::string& path) {
    std::string cmd = "strip --strip-unneeded \"" + path + "\" 2>/dev/null";
    system(cmd.c_str());
}

static std::string resolve_lib(const std::string& name) {
    if (!name.empty() && name[0] == '/') return name;

    const char* dirs[] = {
        "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
        "/lib64", "/usr/lib64",
        "/lib", "/usr/lib",
        "/usr/local/lib", "/usr/local/lib64",
        nullptr
    };
    for (const char** d = dirs; *d; ++d) {
        std::string candidate = std::string(*d) + "/" + name;
        struct stat st;
        if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            return candidate;
    }
    return "";
}

static std::string dirname_of(const std::string& path) {
    size_t pos = path.rfind('/');
    return pos != std::string::npos ? path.substr(0, pos) : ".";
}

static std::string basename_of(const std::string& path) {
    size_t pos = path.rfind('/');
    return pos != std::string::npos ? path.substr(pos + 1) : path;
}

static std::vector<std::string> ldd_parse(const std::string& elf_path) {
    std::vector<std::string> result;
    std::string cmd = "ldd \"" + elf_path + "\" 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;

    char line[1024];
    while (fgets(line, sizeof(line), pipe)) {
        std::regex re(R"((/\S+\.so\S*)\s)");
        std::smatch m;
        std::string s(line);
        auto it = s.cbegin();
        while (std::regex_search(it, s.cend(), m, re)) {
            result.push_back(m[1].str());
            it = m[0].second;
        }
    }
    pclose(pipe);
    return result;
}

static std::string get_interp(const std::string& elf_path) {
    std::string cmd = "readelf -l \"" + elf_path + "\" 2>/dev/null | grep 'interpreter:'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    char line[512];
    std::string result;
    if (fgets(line, sizeof(line), pipe)) {
        std::regex re(R"(\[(.*)\])");
        std::smatch m;
        std::string s(line);
        if (std::regex_search(s, m, re)) {
            result = m[1].str();
        }
    }
    pclose(pipe);
    return result;
}

// Copy a host binary plus its full shared-library closure into the staged
// initramfs at `dest`.  `seen` dedupes against os.elf's libs.
static void collect_tool(const std::string& host_path, const std::string& dest,
                         const std::string& root, bool do_strip,
                         std::set<std::string>& seen) {
    struct stat st;
    if (stat(host_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;

    copy_file(host_path, root + dest);
    if (do_strip) strip_file(root + dest);

    for (const auto& lib : ldd_parse(host_path)) {
        if (lib.empty() || lib[0] != '/') continue;
        if (stat(lib.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!seen.insert(lib).second) continue;

        std::string dst = root + lib;
        copy_file(lib, dst);
        if (do_strip) strip_file(dst);
    }
}

void collect_deps(const ElfInfo& elf, const std::string& elf_path,
                  const std::string& root, bool do_strip) {
    std::set<std::string> seen;

    if (elf.is_static) {
        copy_file(elf_path, root + "/bin/os.elf");
    } else {
        // dynamic ELF: copy binary and its dynamic linker
        copy_file(elf_path, root + "/bin/os.elf");

        std::string interp = elf.interp.empty() ? get_interp(elf_path) : elf.interp;
        if (!interp.empty()) {
            struct stat st;
            if (stat(interp.c_str(), &st) == 0) {
                copy_file(interp, root + interp);
                seen.insert(interp);
            }
        }

        // copy exactly the shared libraries the loader will pull in: ldd
        // gives the full transitive NEEDED closure as resolved absolute
        // paths (the SONAME files).  Copy each verbatim so the runtime
        // names resolve; no need to stage entire system lib directories.
        for (const auto& lib : ldd_parse(elf_path)) {
            if (lib.empty() || lib[0] != '/') continue;
            struct stat st;
            if (stat(lib.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (!seen.insert(lib).second) continue;

            std::string dst = root + lib;
            copy_file(lib, dst);
            if (do_strip) strip_file(dst);
        }

        if (do_strip) strip_file(root + "/bin/os.elf");
    }

    // Stage the tools the default init script runs: a /bin/sh for the
    // shebang and /bin/mount for the mounts.  Their libs share `seen` so
    // nothing is duplicated.  /bin/sh is a symlink to dash on Debian;
    // copy_file follows it, so dest holds a real executable.
    collect_tool("/bin/sh", "/bin/sh", root, do_strip, seen);
    struct stat mst;
    const char* mount_src = stat("/bin/mount", &mst) == 0 ? "/bin/mount"
                                                        : "/usr/bin/mount";
    if (stat(mount_src, &mst) == 0 && S_ISREG(mst.st_mode))
        collect_tool(mount_src, "/bin/mount", root, do_strip, seen);

    if (stat("/bin/mkdir", &mst) == 0 && S_ISREG(mst.st_mode))
        collect_tool("/bin/mkdir", "/bin/mkdir", root, do_strip, seen);
}

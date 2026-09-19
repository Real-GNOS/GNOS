#include "initramfs.h"
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static uint32_t cpio_ino_counter = 1;

static uint32_t to_be32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

static void cpio_newc_hdr(char* buf, uint32_t ino, uint32_t mode,
                           uint32_t size, const char* name, uint16_t nsize) {
    memset(buf, '0', 110);
    memcpy(buf, "070701", 6);           // magic
    snprintf(buf + 6,   8, "%08x", ino);
    snprintf(buf + 14,  8, "%08x", mode);
    snprintf(buf + 22,  8, "%08x", 0);  // uid
    snprintf(buf + 30,  8, "%08x", 0);  // gid
    snprintf(buf + 38,  8, "%08x", 1);  // nlink
    snprintf(buf + 46,  8, "%08x", 0);  // mtime
    snprintf(buf + 54,  8, "%08x", size);
    snprintf(buf + 62,  8, "%08x", 0);  // devmajor
    snprintf(buf + 70,  8, "%08x", 0);  // devminor
    snprintf(buf + 78,  8, "%08x", 0);  // rdevmajor
    snprintf(buf + 86,  8, "%08x", 0);  // rdevminor
    snprintf(buf + 94,  8, "%08x", nsize + 1);  // namesize
    snprintf(buf + 102, 8, "%08x", 0);  // check
}

static size_t pad4(size_t v) { return (v + 3) & ~3; }

void InitramfsBuilder::add_dir(const std::string& path, mode_t mode) {
    Entry e;
    e.cpio_path = path;
    e.mode = mode | 0040000;
    e.is_dir = true;
    e.is_mem = false;
    e.is_link = false;
    entries_.push_back(e);
}

void InitramfsBuilder::add_file(const std::string& path, const std::string& host_src, mode_t mode) {
    Entry e;
    e.cpio_path = path;
    e.host_path = host_src;
    e.mode = mode | 0100000;
    e.is_dir = false;
    e.is_mem = false;
    e.is_link = false;
    entries_.push_back(e);
}

void InitramfsBuilder::add_mem_file(const std::string& path, const void* data, size_t len, mode_t mode) {
    Entry e;
    e.cpio_path = path;
    e.mem_data.assign((const char*)data, len);
    e.mode = mode | 0100000;
    e.is_dir = false;
    e.is_mem = true;
    e.is_link = false;
    entries_.push_back(e);
}

void InitramfsBuilder::add_script(const std::string& path, const std::string& content, mode_t mode) {
    add_mem_file(path, content.data(), content.size(), mode | 0100000);
}

bool InitramfsBuilder::write(const std::string& output_path) {
    std::string cpio_buf;

    // add directories first
    for (const auto& e : entries_) {
        if (!e.is_dir) continue;
        std::string name = "./" + e.cpio_path;
        if (name.back() != '/') name += '/';
        char hdr[110];
        cpio_newc_hdr(hdr, cpio_ino_counter++, e.mode, 0, name.c_str(), name.size());
        cpio_buf.append(hdr, 110);
        cpio_buf.append(name);
        size_t pad = pad4(110 + name.size()) - (110 + name.size());
        cpio_buf.append(pad, '\0');
    }

    // add files
    for (const auto& e : entries_) {
        if (e.is_dir) continue;

        std::string name = "./" + e.cpio_path;
        std::string content;
        if (e.is_mem) {
            content = e.mem_data;
        } else {
            std::ifstream in(e.host_path, std::ios::binary);
            if (!in) continue;
            content.assign(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
        }

        char hdr[110];
        cpio_newc_hdr(hdr, cpio_ino_counter++, e.mode, content.size(),
                       name.c_str(), name.size());
        cpio_buf.append(hdr, 110);
        cpio_buf.append(name);
        size_t pad = pad4(110 + name.size()) - (110 + name.size());
        cpio_buf.append(pad, '\0');
        cpio_buf.append(content);
        pad = pad4(content.size()) - content.size();
        cpio_buf.append(pad, '\0');
    }

    // TRAILER
    const char* trailer = "TRAILER!!!";
    char hdr[110];
    cpio_newc_hdr(hdr, 0, 0, 0, trailer, strlen(trailer));
    cpio_buf.append(hdr, 110);
    cpio_buf.append(trailer, strlen(trailer) + 1);
    size_t pad = pad4(110 + strlen(trailer) + 1) - (110 + strlen(trailer) + 1);
    cpio_buf.append(pad, '\0');

    // gzip and write
    gzFile gz = gzopen(output_path.c_str(), "wb9");
    if (!gz) return false;
    gzwrite(gz, cpio_buf.data(), cpio_buf.size());
    gzclose(gz);
    return true;
}

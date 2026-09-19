#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class InitramfsBuilder {
public:
    void add_dir(const std::string& path, mode_t mode = 0755);
    void add_file(const std::string& path, const std::string& host_src, mode_t mode = 0644);
    void add_mem_file(const std::string& path, const void* data, size_t len, mode_t mode = 0644);
    void add_script(const std::string& path, const std::string& content, mode_t mode = 0755);
    bool write(const std::string& output_path);

private:
    struct Entry {
        std::string cpio_path;
        std::string host_path;
        std::string mem_data;
        uint32_t mode;
        bool is_dir;
        bool is_mem;
        bool is_link;
        std::string link_target;
    };
    std::vector<Entry> entries_;

    static void cpio_newc_header(char* buf, uint32_t ino, uint32_t mode,
                                 uint32_t size, const char* name, uint16_t name_size);
};

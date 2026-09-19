#pragma once

#include <string>
#include <utility>
#include <vector>

/* Advanced ("pro") options shared by both the CLI and the GUI.
 *
 * These are hidden behind --pro / the "Pro options" toggle: they unlock
 * knobs the casual user does not need, and most of them require a working
 * initramfs understanding (custom kernel cmdline, extra payload files,
 * overwriting what /init executes, compression tuning).
 */
struct ProOptions {
    bool enabled = false;

    /* Kernel command line for the boot entry.
     * Default: "console=tty0 init=/init quiet" */
    std::string kcmd = "console=tty0 init=/init quiet";

    /* gzip level used when packing the initramfs, 1..9 */
    int gzip_level = 9;

    /* Extra host files to copy into the initramfs: {host_path, tar_path} */
    std::vector<std::pair<std::string, std::string>> extra_files;

    /* What /init runs once the OS is booted.  Defaults to running the
     * packaged ELF. */
    std::string init_cmd = "exec /bin/os.elf";

    /* Keep the temporary build dirs / archive for inspection. */
    bool keep_tmp = false;
};
/*
 * main.cpp — gnoscfg entry point.  C++20.
 *
 * Usage:
 *   gnoscfg               — interactive menuconfig (ncursesw TUI)
 *   gnoscfg --text        — text-mode Q&A (make config)
 *   gnoscfg --save FILE   — save current config to FILE
 *   gnoscfg --load FILE   — load config from FILE
 *   gnoscfg --show        — print all options (make allyesconfig)
 */
#include "kconfig.h"
#include <cstdio>
#include <cstring>
#include <string>

static void usage() {
    printf("gnoscfg — GNOS 内核配置工具\n\n");
    printf("用法:\n");
    printf("  gnoscfg                 交互式菜单配置 (ncursesw)\n");
    printf("  gnoscfg --text          文本模式问答 (make config)\n");
    printf("  gnoscfg --load FILE     加载已有配置\n");
    printf("  gnoscfg --save FILE     保存当前配置\n");
    printf("  gnoscfg --show          显示所有选项\n");
}

int main(int argc, char **argv) {
    gnoscfg::KconfigParser parser;
    auto root = parser.parse("src/gnoscfg/Kconfig");
    if (!root) {
        fprintf(stderr, "错误: 无法解析 src/gnoscfg/Kconfig\n");
        return 1;
    }

    bool text_mode  = false;
    bool show_mode  = false;
    std::string load_file;
    std::string save_file;

    for (int i = 1; i < argc; i++) {
        if (std::string_view(argv[i]) == "--text") {
            text_mode = true;
        } else if (std::string_view(argv[i]) == "--show") {
            show_mode = true;
        } else if (std::string_view(argv[i]) == "--load" && i + 1 < argc) {
            load_file = argv[++i];
        } else if (std::string_view(argv[i]) == "--save" && i + 1 < argc) {
            save_file = argv[++i];
        } else if (std::string_view(argv[i]) == "--help" ||
                   std::string_view(argv[i]) == "-h") {
            usage();
            return 0;
        }
    }

    /* Load existing config if available */
    if (!load_file.empty()) {
        if (parser.load_config(load_file))
            printf("已加载配置: %s\n", load_file.c_str());
    } else {
        parser.load_config("build/.config");
    }

    /* Show mode: dump all options */
    if (show_mode) {
        for (auto &[name, entry] : parser.entries()) {
            printf("CONFIG_%s=%s", name.c_str(), entry->current.raw.c_str());
            if (!entry->prompt.empty())
                printf("  # %s", entry->prompt.c_str());
            printf("\n");
        }
        return 0;
    }

    /* Text mode */
    if (text_mode) {
        gnoscfg::MenuConfig menu(root.get(), parser);
        menu.run_text();
        return 0;
    }

    /* Interactive menuconfig */
    if (!save_file.empty())
        return gnoscfg::MenuConfig(root.get(), parser).run_tui();

    return gnoscfg::MenuConfig(root.get(), parser).run_tui();
}

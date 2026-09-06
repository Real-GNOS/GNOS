/*
 * kconfig.h — GNOS kernel configuration system.  C++20 + ncursesw.
 *
 * Parses a Linux-Kconfig-compatible file into a tree of Menu/Config nodes,
 * then drives a menuconfig-style interactive TUI or text-mode Q&A.  The
 * generated .config is consumed by `make` to control kernel compilation.
 */
#ifndef GNOSCFG_KCONFIG_H
#define GNOSCFG_KCONFIG_H

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/* ncurses: forward-declare the opaque struct outside any namespace so it
   matches the global `struct _win_st` that curses.h defines. */
struct _win_st;

namespace gnoscfg {

/* ------------------------------------------------------------------ */
/* Value types                                                         */
/* ------------------------------------------------------------------ */

enum class OptType : uint8_t { Bool, Tristate, Int, Hex, String };

struct OptValue {
    OptType type = OptType::Bool;
    std::string raw;
    bool touched = false;

    bool        as_bool()    const { return raw == "y" || raw == "Y"; }
    int         as_int()     const;
    std::string as_string()  const { return raw; }

    std::string display() const;
    void toggle();
};

/* ------------------------------------------------------------------ */
/* Menu tree nodes                                                     */
/* ------------------------------------------------------------------ */

struct ConfigEntry {
    std::string name;
    OptType     type    = OptType::Bool;
    std::string defval;
    std::string prompt;
    std::string help;
    std::vector<std::string> depends;
    OptValue    current;
};

struct MenuNode {
    enum Kind { Submenu, Config, Comment };

    Kind        kind = Submenu;
    std::string title;
    std::string comment_text;
    std::string config_name;
    ConfigEntry *entry = nullptr;

    std::vector<std::unique_ptr<MenuNode>> children;
    MenuNode *parent = nullptr;
};

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

class KconfigParser {
public:
    std::unique_ptr<MenuNode> parse(std::string_view path);

    ConfigEntry *find(std::string_view name);
    const std::unordered_map<std::string, std::unique_ptr<ConfigEntry>> &
        entries() const { return m_entries; }

    bool load_config(std::string_view path);
    void save_config(std::string_view path) const;

private:
    void parse_block(FILE *fp, MenuNode *parent,
                     std::vector<std::string> &dep_stack);
    ConfigEntry *ensure(std::string_view name);

    std::unordered_map<std::string, std::unique_ptr<ConfigEntry>> m_entries;
};

/* ------------------------------------------------------------------ */
/* TUI — menuconfig-style ncursesw interface                          */
/* ------------------------------------------------------------------ */

class MenuConfig {
public:
    MenuConfig(MenuNode *root, KconfigParser &parser);
    ~MenuConfig();

    int  run_tui();           /* interactive ncurses menuconfig */
    void run_text();          /* text-mode Q&A (make config)    */

private:
    /* Drawing */
    void redraw();
    void draw_border();
    void draw_title();
    void draw_status();
    void draw_help();
    void draw_menu();

    /* Navigation */
    void enter_submenu();
    void go_parent();
    void toggle_value();
    void clamp_cursor();

    /* Dependency evaluation */
    bool is_visible(const ConfigEntry *e) const;
    int  count_visible() const;
    MenuNode *nth_visible(int n) const;

    /* Colors */
    enum : short {
        CP_TITLE=1, CP_MENU, CP_SELECT, CP_BORDER, CP_HELP,
        CP_STATUS, CP_BOOL_ON, CP_BOOL_OFF, CP_COMMENT, CP_HEADER,
        CP_SUBMENU, CP_STRING,
    };
    void init_colors();

    MenuNode      *m_root;
    KconfigParser &m_parser;

    int  m_cursor   = 0;
    int  m_scroll   = 0;
    bool m_dirty    = true;
    bool m_running  = true;
    bool m_changed  = false;

    MenuNode       *m_cur_menu = nullptr;
    std::vector<MenuNode *> m_stack;

    /* ncurses windows — typed as global struct _win_st* so they match
       the return type of newwin() from curses.h. */
    struct _win_st *w_title  = nullptr;
    struct _win_st *w_border = nullptr;
    struct _win_st *w_menu   = nullptr;
    struct _win_st *w_help   = nullptr;
    struct _win_st *w_status = nullptr;
};

} /* namespace gnoscfg */

#endif /* GNOSCFG_KCONFIG_H */

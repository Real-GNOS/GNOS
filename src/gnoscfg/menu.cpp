/*
 * menu.cpp — menuconfig-style ncursesw TUI for gnoscfg.  C++20.
 *
 * ┌──────────────────────────────────────────────────────────────┐
 * │                 GNOS 内核配置 (make config)                  │
 * ├──────────────────────────────────────────────────────────────┤
 * │ > ─── 核心系统 (Core) ──────────────────────────────── --->  │
 * │   ─── 调度与进程 (Scheduling & Process) ───────────── --->  │
 * │   ─── 文件系统 (Filesystem) ───────────────────────── --->  │
 * │   ─── 设备驱动 (Drivers) ──────────────────────────── --->  │
 * │   ...                                                      │
 * │                                                             │
 * │                 <Select>  <Save>  <Exit>                    │
 * ├──────────────────────────────────────────────────────────────┤
 * │ 帮助: 启用多处理器支持                                      │
 * ├──────────────────────────────────────────────────────────────┤
 * │ gnoscfg [1/6]  F2=Save  F9=Quit  Enter=Sub  Space=Toggle   │
 * └──────────────────────────────────────────────────────────────┘
 */
#include "kconfig.h"
#include <curses.h>
#include <locale.h>
#include <cstring>
#include <cstdlib>

namespace gnoscfg {

/* ================================================================== */
/* Colors                                                              */
/* ================================================================== */

void MenuConfig::init_colors() {
    start_color();
    use_default_colors();
    init_pair(CP_TITLE,    COLOR_CYAN,    COLOR_BLUE);
    init_pair(CP_MENU,     COLOR_WHITE,   COLOR_BLUE);
    init_pair(CP_SELECT,   COLOR_YELLOW,  COLOR_BLUE);
    init_pair(CP_BORDER,   COLOR_GREEN,   COLOR_BLUE);
    init_pair(CP_HELP,     COLOR_YELLOW,  COLOR_BLUE);
    init_pair(CP_STATUS,   COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_BOOL_ON,  COLOR_GREEN,   COLOR_BLUE);
    init_pair(CP_BOOL_OFF, COLOR_RED,     COLOR_BLUE);
    init_pair(CP_COMMENT,  COLOR_CYAN,    COLOR_BLUE);
    init_pair(CP_HEADER,   COLOR_WHITE,   COLOR_BLUE);
    init_pair(CP_SUBMENU,  COLOR_MAGENTA, COLOR_BLUE);
    init_pair(CP_STRING,   COLOR_GREEN,   COLOR_BLUE);
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

MenuConfig::MenuConfig(MenuNode *root, KconfigParser &parser)
    : m_root(root), m_parser(parser), m_cur_menu(root) {}

MenuConfig::~MenuConfig() {
    if (w_title)  delwin(w_title);
    if (w_border) delwin(w_border);
    if (w_menu)   delwin(w_menu);
    if (w_help)   delwin(w_help);
    if (w_status) delwin(w_status);
    endwin();
}

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

bool MenuConfig::is_visible(const ConfigEntry *e) const {
    if (!e) return true;
    for (auto &dep : e->depends) {
        if (dep.empty()) continue;
        auto *d = m_parser.find(dep);
        if (d && !d->current.as_bool()) return false;
    }
    return true;
}

int MenuConfig::count_visible() const {
    int n = 0;
    for (auto &ch : m_cur_menu->children) {
        if (ch->kind == MenuNode::Config && ch->entry)
            { if (is_visible(ch->entry)) n++; }
        else
            n++;
    }
    return n;
}

MenuNode *MenuConfig::nth_visible(int n) const {
    int c = 0;
    for (auto &ch : m_cur_menu->children) {
        if (ch->kind == MenuNode::Config && ch->entry) {
            if (!is_visible(ch->entry)) continue;
        }
        if (c == n) return ch.get();
        c++;
    }
    return nullptr;
}

void MenuConfig::clamp_cursor() {
    int total = count_visible();
    if (total == 0) { m_cursor = 0; return; }
    if (m_cursor < 0) m_cursor = 0;
    if (m_cursor >= total) m_cursor = total - 1;
}

/* ================================================================== */
/* Drawing                                                             */
/* ================================================================== */

static void clear_line(WINDOW *w, int y, int x, int len) {
    wmove(w, y, x);
    for (int i = 0; i < len; i++) waddch(w, ' ');
}

void MenuConfig::draw_border() {
    if (!w_border) return;
    wattron(w_border, COLOR_PAIR(CP_BORDER));
    box(w_border, 0, 0);
    wattroff(w_border, COLOR_PAIR(CP_BORDER));
    wrefresh(w_border);
}

void MenuConfig::draw_title() {
    if (!w_title) return;
    werase(w_title);
    int W = getmaxx(w_title);
    wattron(w_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
    const char *t = " GNOS 内核配置 (make config) — gnoscfg ";
    mvwprintw(w_title, 0, (W - (int)strlen(t)) / 2, "%s", t);
    wattroff(w_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
    wrefresh(w_title);
}

void MenuConfig::draw_status() {
    if (!w_status) return;
    werase(w_status);
    int W = getmaxx(w_status);
    wattron(w_status, COLOR_PAIR(CP_STATUS));

    /* Count configs at current menu level */
    int total = count_visible();
    char buf[128];
    snprintf(buf, sizeof(buf), " gnoscfg [%d/%d]  F2=保存  F9=退出  Enter=进入  Space=切换  Backspace=返回",
             m_cursor + 1, total);
    wprintw(w_status, "%s", buf);
    wattroff(w_status, COLOR_PAIR(CP_STATUS));
    wrefresh(w_status);
}

void MenuConfig::draw_help() {
    if (!w_help) return;
    werase(w_help);
    int W = getmaxx(w_help);

    MenuNode *sel = nth_visible(m_cursor);
    if (sel) {
        wattron(w_help, COLOR_PAIR(CP_HELP));
        if (sel->kind == MenuNode::Config && sel->entry) {
            const char *h = sel->entry->help.c_str();
            if (!sel->entry->help.empty()) {
                /* Truncate to window width */
                char buf[512];
                snprintf(buf, sizeof(buf), "帮助: %s", h);
                mvwprintw(w_help, 0, 1, "%-.*s", W - 2, buf);
            } else {
                mvwprintw(w_help, 0, 1, "CONFIG_%s = %s",
                          sel->entry->name.c_str(),
                          sel->entry->current.raw.c_str());
            }
        } else if (sel->kind == MenuNode::Submenu) {
            mvwprintw(w_help, 0, 1, "%-.*s", W - 2, sel->title.c_str());
        } else if (sel->kind == MenuNode::Comment) {
            mvwprintw(w_help, 0, 1, "%-.*s", W - 2,
                      sel->comment_text.c_str());
        }
        wattroff(w_help, COLOR_PAIR(CP_HELP));
    }
    wrefresh(w_help);
}

void MenuConfig::draw_menu() {
    if (!w_menu) return;
    werase(w_menu);
    int W = getmaxx(w_menu) - 2;
    int H = getmaxy(w_menu) - 2;

    clamp_cursor();

    /* Adjust scroll */
    if (m_cursor < m_scroll) m_scroll = m_cursor;
    if (m_cursor >= m_scroll + H) m_scroll = m_cursor - H + 1;

    for (int i = 0; i < H; i++) {
        int idx = m_scroll + i;
        MenuNode *node = nth_visible(idx);
        if (!node) break;

        bool sel = (idx == m_cursor);
        chtype attr = sel ? COLOR_PAIR(CP_SELECT) : COLOR_PAIR(CP_MENU);

        wmove(w_menu, 1 + i, 1);
        /* Clear line */
        wattron(w_menu, attr);
        for (int c = 0; c < W; c++) waddch(w_menu, ' ');
        wmove(w_menu, 1 + i, 1);

        switch (node->kind) {

        case MenuNode::Comment:
            wattroff(w_menu, attr);
            wattron(w_menu, COLOR_PAIR(CP_COMMENT) | A_BOLD);
            wprintw(w_menu, "  --- %s ---", node->comment_text.c_str());
            wattroff(w_menu, COLOR_PAIR(CP_COMMENT));
            break;

        case MenuNode::Submenu: {
            wattroff(w_menu, attr);
            chtype mattr = sel ? COLOR_PAIR(CP_SELECT) : COLOR_PAIR(CP_SUBMENU);
            wattron(w_menu, mattr);
            wprintw(w_menu, "%c %s",
                    sel ? '>' : ' ', node->title.c_str());
            wattroff(w_menu, mattr);
            /* Arrow on right */
            wattron(w_menu, COLOR_PAIR(CP_SUBMENU));
            mvwprintw(w_menu, 1 + i, W - 4, " --->");
            wattroff(w_menu, COLOR_PAIR(CP_SUBMENU));
            break;
        }

        case MenuNode::Config: {
            ConfigEntry *e = node->entry;
            const char *prompt = e->prompt.empty() ? e->name.c_str()
                                                   : e->prompt.c_str();
            switch (e->type) {
            case OptType::Bool:
            case OptType::Tristate: {
                bool on = e->current.as_bool();
                chtype vattr = on ? COLOR_PAIR(CP_BOOL_ON) : COLOR_PAIR(CP_BOOL_OFF);
                if (sel) vattr = COLOR_PAIR(CP_SELECT) | A_BOLD;
                wattroff(w_menu, attr);
                wattron(w_menu, vattr);
                wprintw(w_menu, " [%s] ", on ? "*" : " ");
                wattroff(w_menu, vattr);
                wattron(w_menu, attr);
                int maxlen = W - 8;
                wprintw(w_menu, "%-.*s", maxlen, prompt);
                /* Right-aligned value indicator */
                const char *val = e->current.display().c_str();
                int vlen = (int)strlen(val);
                if (vlen > 0 && vlen < W - 25) {
                    wattroff(w_menu, attr);
                    wattron(w_menu, COLOR_PAIR(CP_BOOL_ON));
                    mvwprintw(w_menu, 1 + i, W - vlen - 1, "%s", val);
                    wattroff(w_menu, COLOR_PAIR(CP_BOOL_ON));
                }
                break;
            }
            case OptType::Int:
            case OptType::Hex:
            case OptType::String: {
                wattroff(w_menu, attr);
                wattron(w_menu, attr);
                wprintw(w_menu, "     %-.*s", W - 25, prompt);
                wattroff(w_menu, attr);
                /* Right-aligned value */
                std::string val = e->current.display();
                wattron(w_menu, COLOR_PAIR(CP_STRING));
                mvwprintw(w_menu, 1 + i, W - (int)val.size() - 1,
                          "%s", val.c_str());
                wattroff(w_menu, COLOR_PAIR(CP_STRING));
                break;
            }
            } /* switch type */
            break;
        }
        } /* switch kind */

        wattroff(w_menu, attr);
    }

    /* Bottom bar */
    wattron(w_menu, COLOR_PAIR(CP_HEADER) | A_BOLD);
    clear_line(w_menu, H + 1, 1, W);
    mvwprintw(w_menu, H + 1, 2, "<选择>  <保存>  <退出>");
    wattroff(w_menu, COLOR_PAIR(CP_HEADER));

    wrefresh(w_menu);
}

void MenuConfig::redraw() {
    draw_border();
    draw_title();
    draw_menu();
    draw_help();
    draw_status();
}

/* ================================================================== */
/* Navigation                                                          */
/* ================================================================== */

void MenuConfig::enter_submenu() {
    MenuNode *sel = nth_visible(m_cursor);
    if (!sel || sel->kind != MenuNode::Submenu) return;

    m_stack.push_back(m_cur_menu);
    m_cur_menu = sel;
    m_cursor = 0;
    m_scroll = 0;
}

void MenuConfig::go_parent() {
    if (m_stack.empty()) return;
    m_cur_menu = m_stack.back();
    m_stack.pop_back();
    m_cursor = 0;
    m_scroll = 0;
}

void MenuConfig::toggle_value() {
    MenuNode *sel = nth_visible(m_cursor);
    if (!sel || sel->kind != MenuNode::Config || !sel->entry) return;
    sel->entry->current.toggle();
    m_changed = true;
}

/* ================================================================== */
/* TUI main loop                                                       */
/* ================================================================== */

int MenuConfig::run_tui() {
    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int menu_h = max_y - 6;
    int menu_w = max_x - 4;
    if (menu_h < 3) menu_h = 3;
    if (menu_w < 20) menu_w = 20;

    w_title  = newwin(1, max_x, 0, 0);
    w_border = newwin(menu_h + 2, menu_w + 2, 2, 1);
    w_menu   = newwin(menu_h, menu_w, 3, 2);
    w_help   = newwin(1, max_x, max_y - 3, 0);
    w_status = newwin(1, max_x, max_y - 1, 0);

    redraw();

    while (m_running) {
        redraw();
        int ch = getch();

        switch (ch) {
        case KEY_UP:    case 'k': m_cursor--; break;
        case KEY_DOWN:  case 'j': m_cursor++; break;
        case KEY_PPAGE: m_cursor -= menu_h;  break;
        case KEY_NPAGE: m_cursor += menu_h;  break;
        case '\n': case KEY_ENTER: enter_submenu(); break;
        case ' ':                   toggle_value();  break;
        case KEY_BACKSPACE: case 127: go_parent(); break;
        case KEY_F(2):
            m_parser.save_config("build/.config");
            m_changed = false;
            break;
        case KEY_F(9): case 'q': case 27:
            m_running = false;
            break;
        default: break;
        }
    }

    return m_changed ? 0 : 1;
}

/* ================================================================== */
/* Text mode                                                           */
/* ================================================================== */

void MenuConfig::run_text() {
    printf("\033[1;36mGNOS 内核配置 (文本模式)\033[0m\n\n");

    std::function<void(MenuNode *, int)> walk = [&](MenuNode *node, int depth) {
        for (auto &ch : node->children) {
            if (ch->kind == MenuNode::Comment) {
                printf("%*s\033[1;37m--- %s ---\033[0m\n",
                       depth * 2, "", ch->comment_text.c_str());
            } else if (ch->kind == MenuNode::Submenu) {
                printf("\n%s\033[1;35m%s:\033[0m\n",
                       std::string(depth * 2, ' ').c_str(),
                       ch->title.c_str());
                walk(ch.get(), depth + 1);
            } else if (ch->kind == MenuNode::Config && ch->entry) {
                ConfigEntry *e = ch->entry;
                if (!is_visible(e)) continue;

                const char *prompt = e->prompt.empty() ? e->name.c_str()
                                                       : e->prompt.c_str();
                printf("%s\033[1;33m%s\033[0m (%s) [%s]: ",
                       std::string(depth * 2, ' ').c_str(),
                       prompt, e->name.c_str(), e->current.raw.c_str());

                char buf[512] = "";
                if (fgets(buf, sizeof(buf), stdin)) {
                    size_t len = strlen(buf);
                    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
                        buf[--len] = '\0';
                    if (buf[0] != '\0') {
                        e->current.raw = buf;
                        e->current.touched = true;
                    }
                }
            }
        }
    };

    walk(m_root, 0);

    m_parser.save_config("build/.config");
    printf("\n\033[1;32m配置已保存到 build/.config\033[0m\n");
}

} /* namespace gnoscfg */

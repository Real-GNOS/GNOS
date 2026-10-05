#include "kernel/gui/clients_internal.h"

#define EDIT_ROWS 20
#define EDIT_COLS 76
#define EDIT_LINE_H 18

struct EDIT_STATE {
    struct COMP_DEMO_CLIENT dc;
    char lines[EDIT_ROWS][EDIT_COLS];
    int len[EDIT_ROWS];
    int cur_l;
    int cur_c;
    int scroll;
    int dirty;
};

static void edit_load(struct EDIT_STATE *ed) {
    struct FS_STAT st;
    if (sys_stat("/note.txt", &st) != 0 || st.st_size == 0 ||
        st.st_size > 32768)
        return;
    int fd = open_file("/note.txt", O_RDONLY);
    if (fd < 0)
        return;
    static char buf[16384];
    uint32_t got = read_file(fd, buf, st.st_size);
    close_file(fd);
    int l = 0;
    int c = 0;
    for (uint32_t i = 0; i < got; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            ed->lines[l][c] = 0;
            ed->len[l] = c;
            l++;
            c = 0;
            if (l >= EDIT_ROWS)
                break;
            continue;
        }
        if (c < EDIT_COLS - 1)
            ed->lines[l][c++] = ch;
    }
    ed->lines[l][c] = 0;
    ed->len[l] = c;
    for (int k = l + 1; k < EDIT_ROWS; k++) {
        ed->lines[k][0] = 0;
        ed->len[k] = 0;
    }
    ed->cur_l = 0;
    ed->cur_c = 0;
    ed->scroll = 0;
}

static void edit_save(struct EDIT_STATE *ed) {
    sys_unlink("/note.txt");
    create_file("/note.txt");
    int fd = open_file("/note.txt", O_WRONLY);
    if (fd < 0) {
        kprintf("edit: save open failed\n");
        return;
    }
    for (int k = 0; k < EDIT_ROWS; k++) {
        if (ed->len[k] > 0)
            write_file(fd, ed->lines[k], (uint32_t)ed->len[k]);
        write_file(fd, "\n", 1);
    }
    close_file(fd);
    ed->dirty = 0;
    kprintf("edit: saved /note.txt\n");
}

static void edit_render(struct COMP_DEMO_CLIENT *dc) {
    struct EDIT_STATE *ed = (struct EDIT_STATE *)dc;
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);
    int vis = (dc->h - 40) / EDIT_LINE_H;
    if (vis < 1)
        vis = 1;
    for (int k = 0; k < vis && ed->scroll + k < EDIT_ROWS; k++) {
        int l = ed->scroll + k;
        font_draw(&cv, 10, 6 + k * EDIT_LINE_H, ed->lines[l], UI_FONT_PX,
                  theme()->text);
        if (l == ed->cur_l) {
            char pre[EDIT_COLS];
            s_copy(pre, ed->lines[l], EDIT_COLS);
            if (ed->cur_c < ed->len[l])
                pre[ed->cur_c] = 0;
            int cw = font_text_width(pre, UI_FONT_PX);
            gfx_fill(&cv, 10 + cw, 6 + k * EDIT_LINE_H, 2,
                     EDIT_LINE_H - 4, theme()->accent);
        }
    }
    int fy = dc->h - 22;
    gfx_fill(&cv, 0, fy, dc->w, 22, theme()->bar);
    char foot[48];
    char num[12];
    foot[0] = 0;
    strcat(foot, "/note.txt  Ctrl+S save  Ln ");
    u32_to_dec((uint32_t)(ed->cur_l + 1), num);
    strcat(foot, num);
    strcat(foot, " Col ");
    u32_to_dec((uint32_t)(ed->cur_c + 1), num);
    strcat(foot, num);
    if (ed->dirty)
        strcat(foot, "  *");
    font_draw(&cv, 6, fy + 4, foot, UI_FONT_PX, theme()->muted);
}

static void edit_ensure_visible(struct EDIT_STATE *ed,
                                struct COMP_DEMO_CLIENT *dc) {
    int vis = (dc->h - 40) / EDIT_LINE_H;
    if (vis < 1)
        vis = 1;
    if (ed->cur_l < ed->scroll)
        ed->scroll = ed->cur_l;
    if (ed->cur_l >= ed->scroll + vis)
        ed->scroll = ed->cur_l - vis + 1;
}

static void edit_on_key(struct COMP_DEMO_CLIENT *dc, int sc, int mods) {
    struct EDIT_STATE *ed = (struct EDIT_STATE *)dc;
    if ((mods & KBD_MOD_CTRL) && !(mods & KBD_MOD_ALT) && sc == KBD_SC_S) {
        edit_save(ed);
        client_repaint(dc);
        return;
    }
    if (sc == KBD_SC_BACKSPACE) {
        if (ed->cur_c > 0) {
            ed->cur_c--;
            for (int i = ed->cur_c; i < ed->len[ed->cur_l]; i++)
                ed->lines[ed->cur_l][i] = ed->lines[ed->cur_l][i + 1];
            ed->len[ed->cur_l]--;
            ed->dirty = 1;
        } else if (ed->cur_l > 0) {
            int prev = ed->cur_l - 1;
            int room = EDIT_COLS - 1 - ed->len[prev];
            int move = ed->len[ed->cur_l] < room ? ed->len[ed->cur_l] : room;
            for (int i = 0; i < move; i++)
                ed->lines[prev][ed->len[prev] + i] = ed->lines[ed->cur_l][i];
            ed->cur_c = ed->len[prev];
            ed->len[prev] += move;
            for (int k = ed->cur_l; k < EDIT_ROWS - 1; k++) {
                s_copy(ed->lines[k], ed->lines[k + 1], EDIT_COLS);
                ed->len[k] = ed->len[k + 1];
            }
            ed->cur_l--;
            ed->dirty = 1;
        }
    } else if (sc == KBD_SC_ENTER) {
        if (ed->cur_l + 1 < EDIT_ROWS) {
            for (int k = EDIT_ROWS - 1; k > ed->cur_l; k--) {
                s_copy(ed->lines[k], ed->lines[k - 1], EDIT_COLS);
                ed->len[k] = ed->len[k - 1];
            }
            int tail = ed->len[ed->cur_l] - ed->cur_c;
            s_copy(ed->lines[ed->cur_l + 1], ed->lines[ed->cur_l] + ed->cur_c,
                   EDIT_COLS);
            ed->len[ed->cur_l + 1] = tail > 0 ? tail : 0;
            ed->lines[ed->cur_l][ed->cur_c] = 0;
            ed->len[ed->cur_l] = ed->cur_c;
            ed->cur_l++;
            ed->cur_c = 0;
            ed->dirty = 1;
        }
    } else if (sc == KBD_SC_UP) {
        if (ed->cur_l > 0)
            ed->cur_l--;
        if (ed->cur_c > ed->len[ed->cur_l])
            ed->cur_c = ed->len[ed->cur_l];
    } else if (sc == KBD_SC_DOWN) {
        if (ed->cur_l + 1 < EDIT_ROWS)
            ed->cur_l++;
        if (ed->cur_c > ed->len[ed->cur_l])
            ed->cur_c = ed->len[ed->cur_l];
    } else if (sc == KBD_SC_LEFT) {
        if (ed->cur_c > 0)
            ed->cur_c--;
    } else if (sc == KBD_SC_RIGHT) {
        if (ed->cur_c < ed->len[ed->cur_l])
            ed->cur_c++;
    } else {
        char ch = sc_to_char(sc, mods & 1);
        if (ch && ed->len[ed->cur_l] < EDIT_COLS - 1) {
            for (int i = ed->len[ed->cur_l]; i > ed->cur_c; i--)
                ed->lines[ed->cur_l][i] = ed->lines[ed->cur_l][i - 1];
            ed->lines[ed->cur_l][ed->cur_c++] = ch;
            ed->len[ed->cur_l]++;
            ed->dirty = 1;
        } else {
            return;
        }
    }
    edit_ensure_visible(ed, dc);
    client_repaint(dc);
}

static const struct CLIENT_DESC edit_desc = {"edit", "edit",
                                             edit_render, edit_on_key,
                                             1000000};

void clients_edit_thread(void *arg) {
    (void)arg;
    struct EDIT_STATE ed;
    memset(&ed, 0, sizeof(ed));
    edit_load(&ed);
    if (client_begin(&ed.dc, &edit_desc) != 0) {
        thread_exit_current();
        return;
    }
    client_main(&ed.dc);
}

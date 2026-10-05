#include "kernel/gui/clients_internal.h"

#define FILES_MAX 64
#define FILES_NAME_LEN 26
#define FILES_ROW_H 22

struct FILES_STATE {
    struct COMP_DEMO_CLIENT dc;
    char cwd[MAX_PATH_LEN];
    char names[FILES_MAX][FILES_NAME_LEN];
    uint8_t is_dir[FILES_MAX];
    uint32_t sizes[FILES_MAX];
    int count;
    int sel;
    int scroll;
};

static void files_load(struct FILES_STATE *fs) {
    fs->count = 0;
    fs->sel = 0;
    fs->scroll = 0;
    struct FS_DIR *d = sys_opendir(fs->cwd);
    if (!d)
        return;
    struct FS_DIRENT *e;
    while (fs->count < FILES_MAX && (e = sys_readdir(d)) != 0) {
        const char *nm = e->filename;
        if (nm[0] == '.' && nm[1] == 0)
            continue;
        int i = fs->count;
        s_copy(fs->names[i], nm, FILES_NAME_LEN);
        fs->is_dir[i] = (e->f_type == FT_DIRECTORY);
        fs->sizes[i] = 0;
        if (!fs->is_dir[i]) {
            char p[MAX_PATH_LEN];
            s_join(p, fs->cwd, fs->names[i], MAX_PATH_LEN);
            struct FS_STAT st;
            if (sys_stat(p, &st) == 0)
                fs->sizes[i] = st.st_size;
        }
        fs->count++;
    }
    sys_closedir(d);
}

static int files_visible(struct COMP_DEMO_CLIENT *dc) {
    int h = dc->h - 52;
    int n = h / FILES_ROW_H;
    return n < 1 ? 1 : n;
}

static void files_enter(struct FILES_STATE *fs) {
    if (fs->count == 0 || !fs->is_dir[fs->sel])
        return;
    char p[MAX_PATH_LEN];
    s_join(p, fs->cwd, fs->names[fs->sel], MAX_PATH_LEN);
    struct FS_STAT st;
    if (sys_stat(p, &st) != 0 || st.st_filetype != FT_DIRECTORY) {
        comp_log("files: open dir failed");
        return;
    }
    s_copy(fs->cwd, p, MAX_PATH_LEN);
    files_load(fs);
}

static void files_up(struct FILES_STATE *fs) {
    int l = strlen(fs->cwd);
    if (l <= 1)
        return;
    while (l > 1 && fs->cwd[l - 1] != '/')
        l--;
    if (l > 1)
        l--;
    fs->cwd[l] = 0;
    files_load(fs);
}

static void files_render(struct COMP_DEMO_CLIENT *dc) {
    struct FILES_STATE *fs = (struct FILES_STATE *)dc;
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);
    int vis = files_visible(dc);
    int head_h = 26;
    gfx_fill(&cv, 0, 0, dc->w, head_h, theme()->bar);
    char head[MAX_PATH_LEN + 16];
    head[0] = 0;
    strcat(head, "dir: ");
    strcat(head, fs->cwd);
    font_draw(&cv, 8, 6, head, UI_FONT_PX, theme()->title_fg_foc);
    for (int k = 0; k < vis; k++) {
        int i = fs->scroll + k;
        if (i >= fs->count)
            break;
        int ry = head_h + 4 + k * FILES_ROW_H;
        if (i == fs->sel)
            gfx_fill_round(&cv, 4, ry, dc->w - 8, FILES_ROW_H - 2, 5,
                           theme()->wp_top);
        gfx_fill_round(&cv, 8, ry + 4, 12, 12, 3,
                       fs->is_dir[i] ? theme()->accent : theme()->dim);
        font_draw(&cv, 28, ry + 3, fs->names[i], UI_FONT_PX,
                  fs->is_dir[i] ? theme()->accent : theme()->text);
        if (!fs->is_dir[i]) {
            char num[12];
            u32_to_dec(fs->sizes[i], num);
            char sz[24];
            sz[0] = 0;
            strcat(sz, num);
            strcat(sz, " B");
            int sw = font_text_width(sz, UI_FONT_PX);
            font_draw(&cv, dc->w - sw - 14, ry + 3, sz, UI_FONT_PX,
                      theme()->muted);
        }
    }
    int fy = dc->h - 22;
    gfx_fill(&cv, 0, fy, dc->w, 22, theme()->bar);
    char foot[MAX_PATH_LEN + 16];
    foot[0] = 0;
    if (fs->count > 0) {
        strcat(foot, fs->names[fs->sel]);
        if (fs->is_dir[fs->sel])
            strcat(foot, "/  Enter: open  BackSpace: up");
        else
            strcat(foot, "  Enter: info  BackSpace: up");
    } else {
        strcat(foot, "empty  BackSpace: up");
    }
    font_draw(&cv, 6, fy + 4, foot, UI_FONT_PX, theme()->muted);
}

static void files_on_key(struct COMP_DEMO_CLIENT *dc, int sc, int mods) {
    struct FILES_STATE *fs = (struct FILES_STATE *)dc;
    (void)mods;
    int vis = files_visible(dc);
    if (sc == KBD_SC_UP && fs->sel > 0)
        fs->sel--;
    else if (sc == KBD_SC_DOWN && fs->sel < fs->count - 1)
        fs->sel++;
    else if (sc == KBD_SC_LEFT)
        fs->sel -= vis;
    else if (sc == KBD_SC_RIGHT)
        fs->sel += vis;
    else if (sc == KBD_SC_ENTER)
        files_enter(fs);
    else if (sc == KBD_SC_BACKSPACE)
        files_up(fs);
    else
        return;
    if (fs->sel >= fs->count)
        fs->sel = fs->count - 1;
    if (fs->sel < 0)
        fs->sel = 0;
    if (fs->sel < fs->scroll)
        fs->scroll = fs->sel;
    if (fs->sel >= fs->scroll + vis)
        fs->scroll = fs->sel - vis + 1;
    client_repaint(dc);
}

static const struct CLIENT_DESC files_desc = {"files", "files",
                                              files_render, files_on_key,
                                              1000000};

void clients_files_thread(void *arg) {
    (void)arg;
    struct FILES_STATE fs;
    memset(&fs, 0, sizeof(fs));
    s_copy(fs.cwd, "/", MAX_PATH_LEN);
    files_load(&fs);
    if (client_begin(&fs.dc, &files_desc) != 0) {
        thread_exit_current();
        return;
    }
    client_main(&fs.dc);
}

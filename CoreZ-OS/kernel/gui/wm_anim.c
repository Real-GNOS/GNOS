#include "kernel/gui/wm_internal.h"

struct WIN_ANIM {
    struct WL_SURFACE *s;
    int target;
};
static struct WIN_ANIM anims[WL_MAX_SURFACES];
static int anim_n;

void wm_anim_start(struct WL_SURFACE *s, int target) {
    for (int i = 0; i < anim_n; i++)
        if (anims[i].s == s)
            return;
    if (anim_n >= WL_MAX_SURFACES)
        return;
    wl_surface_set_alpha(s, 0);
    anims[anim_n].s = s;
    anims[anim_n].target = target;
    anim_n++;
}

int wm_anim_step(void) {
    comp_lock_acquire();
    int progressed = 0;
    for (int i = anim_n - 1; i >= 0; i--) {
        struct WL_SURFACE *s = anims[i].s;
        if (!s || !s->used) {
            anims[i] = anims[--anim_n];
            continue;
        }
        int a = s->alpha + 48;
        if (a > anims[i].target)
            a = anims[i].target;
        wl_surface_set_alpha(s, a);
        progressed = 1;
        if (a == anims[i].target)
            anims[i] = anims[--anim_n];
    }
    comp_lock_release();
    return progressed;
}

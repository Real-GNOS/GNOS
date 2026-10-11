/* SPDX-License-Identifier: GPL-2.0 */
/*
 * audiodev.c — the unified audio device layer. (GPLv2)
 *
 * See audiodev.h for why this exists.  The shape worth repeating here:
 * the only thing that knows the names "ac97" and "hda" is the built-in
 * table at the top of this file, and it is one line per card.  Adding
 * the next codec is a line here (or a direct audiodev_register() call
 * from a late driver init), never a new block in kernel_main.
 */
#include "audiodev.h"

#include "ac97.h"
#include "audio.h"
#include "calsa.h"
#include "debugcon.h"
#include "hda.h"
#include "kstring.h"
#include "subsys.h"

#include <stdint.h>

#define AUDIODEV_MAX 8

static struct audiodev g_devs[AUDIODEV_MAX];
static int             g_count;
static struct audiodev *g_default;

/* ---- streaming adapters ------------------------------------------------
 * The two cards already in the tree speak different verbs below the frame.
 * Flatten both to the audiodev currency: int16 stereo frames in, whole
 * buffer consumed out.
 */

/* AC'97: the engine in audio.c owns the BDL.  Mirror the ALSA PCM path --
 * reset the ring, open the stream, feed, drain -- which is the only
 * sequence proven against the self-test tone. */
static int ac97_play_frames(const int16_t *samples, uint32_t frames)
{
    audio_reset_ring();
    audio_begin();
    int fed = audio_write(samples, frames);
    audio_drain();
    return fed == (int)frames;
}

/* HDA: hda.h declares hda_play() but hda.c has not implemented it yet --
 * the card audits (probe + self-test tone) and stays registered, it just
 * does not take the playback seat.  When hda_play lands, an adapter of
 * the same shape as ac97_play_frames() goes here and the .play entry
 * below fills in. */

/*
 * The built-in card list, probed in order by audiodev_init().  majors
 * match what the old hand-written kernel_main blocks published, so a
 * coldplug script written against the previous boot does not notice.
 */
static const struct audiodev_ops g_builtin[] = {
    {
        .name      = "ac97",
        .major     = 14,
        .minor     = 3,
        .probe     = ac97_init,
        .selftest  = ac97_selftest,
        .play      = ac97_play_frames,
        .drain     = audio_drain,
        /* calsa hooks: straight through to the engine in audio.c */
        .frames_total  = audio_frames_total,
        .in_flight     = audio_in_flight_frames,
        .free_frames   = audio_free_frames,
        .engine_halted = audio_engine_halted,
        .reset_ring    = audio_reset_ring,
        .begin         = audio_begin,
        .stop          = audio_stop,
    },
    {
        .name      = "hda",
        .major     = 116,
        .minor     = 0,
        .probe     = hda_init,
        .selftest  = hda_selftest,
        .play      = NULL,
        .drain     = NULL,
    },
};

/* ---- registry ---------------------------------------------------------- */

int audiodev_register(const struct audiodev_ops *ops)
{
    if (!ops || !ops->name || !ops->probe)
        return -1;
    if (g_count >= AUDIODEV_MAX)
        return -1;

    /* Reject a duplicate name the same way subsys_register() does: two
     * cards claiming "ac97" is a wiring mistake, not a stereo pair. */
    for (int i = 0; i < g_count; i++)
        if (g_devs[i].ops && g_devs[i].ops->name &&
            strcmp(g_devs[i].ops->name, ops->name) == 0)
            return -1;

    struct audiodev *d = &g_devs[g_count];
    d->ops   = ops;
    d->slot  = subsys_register(ops->name, NULL, SUBSYS_CLASS_SOUND,
                               ops->major, ops->minor);
    d->state = SUBSYS_STATE_REGISTERED;
    if (d->slot < 0) {
        d->ops = NULL;
        return -1;
    }

    if (ops->probe()) {
        d->state = SUBSYS_STATE_LIVE;
        subsys_set_state(d->slot, SUBSYS_STATE_LIVE);
        /* First playable card wins the default seat; later cards stay
         * registered and audited, they just do not steal the route. */
        if (!g_default && ops->play)
            g_default = d;
        if (ops->selftest)
            ops->selftest();
        dbg_puts("AUDIODEV: ");
        dbg_puts(ops->name);
        dbg_puts(" LIVE\r\n");
    } else {
        d->state = SUBSYS_STATE_FAILED;
        subsys_set_state(d->slot, SUBSYS_STATE_FAILED);
        dbg_puts("AUDIODEV: ");
        dbg_puts(ops->name);
        dbg_puts(" absent\r\n");
    }

    return g_count++;
}

int audiodev_init(void)
{
    for (unsigned i = 0; i < sizeof g_builtin / sizeof g_builtin[0]; i++)
        audiodev_register(&g_builtin[i]);

    int live = 0;
    for (int i = 0; i < g_count; i++)
        if (g_devs[i].state == SUBSYS_STATE_LIVE)
            live++;

    dbg_puts("AUDIODEV: ");
    dbg_puts_dec((uint32_t)live);
    dbg_puts("/");
    dbg_puts_dec((uint32_t)g_count);
    dbg_puts(" cards live\r\n");
    return live;
}

int audiodev_vfs_register(void)
{
    /* /dev/dsp belongs to the AC'97 engine itself (raw PCM path, not
     * ALSA-shaped).  The /dev/snd pair is calsa: the compat ALSA
     * frontend, which routes through the audiodev hooks below and works
     * for any card that fills them. */
    audio_vfs_register(); /* /dev/dsp            */
    calsa_vfs_register(); /* /dev/snd/controlC0  */
    return g_count;
}

/* ---- introspection and streaming --------------------------------------- */

struct audiodev *audiodev_default(void)
{
    return g_default;
}

int audiodev_count(void)
{
    return g_count;
}

const struct audiodev *audiodev_get(int idx)
{
    if (idx < 0 || idx >= g_count)
        return NULL;
    return &g_devs[idx];
}

int audiodev_play(const int16_t *samples, uint32_t frames)
{
    if (!g_default || !g_default->ops || !g_default->ops->play)
        return 0;
    return g_default->ops->play(samples, frames);
}

void audiodev_drain(void)
{
    if (g_default && g_default->ops && g_default->ops->drain)
        g_default->ops->drain();
}

/* ---- calsa (compat ALSA) engine hooks ---------------------------------- */

#define PCM_HOOK(expr_lacking_card, call)                                     \
    if (!g_default || !g_default->ops || !g_default->ops->call)               \
        return (expr_lacking_card);                                           \
    return g_default->ops->call()

uint32_t audiodev_pcm_frames_total(void)
{
    PCM_HOOK(0u, frames_total);
}

uint32_t audiodev_pcm_in_flight(void)
{
    PCM_HOOK(0u, in_flight);
}

int audiodev_pcm_free_frames(void)
{
    PCM_HOOK(0, free_frames);
}

int audiodev_pcm_halted(void)
{
    PCM_HOOK(0, engine_halted);
}

int audiodev_pcm_reset(void)
{
    if (!g_default || !g_default->ops || !g_default->ops->reset_ring)
        return 0;
    g_default->ops->reset_ring();
    return 1;
}

int audiodev_pcm_begin(void)
{
    if (!g_default || !g_default->ops || !g_default->ops->begin)
        return 0;
    g_default->ops->begin();
    return 1;
}

int audiodev_pcm_stop(void)
{
    if (!g_default || !g_default->ops || !g_default->ops->stop)
        return 0;
    g_default->ops->stop();
    return 1;
}

const char *audiodev_default_name(void)
{
    if (!g_default || !g_default->ops || !g_default->ops->name)
        return "none";
    return g_default->ops->name;
}

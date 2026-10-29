/* SPDX-License-Identifier: GPL-2.0 */
/*
 * audiodev.h — the unified audio device layer. (GPLv2)
 *
 * Every sound card before this file meant one more hand-written block in
 * kernel_main: subsys_register(), the card's init, its self-test, the
 * /dev nodes, and a copy-paste of all of the above for the next card.
 * That is the tax this layer exists to stop paying.  A card driver now
 * describes itself in a struct audiodev_ops and hands it to
 * audiodev_register(); the registry does the subsys bookkeeping, the
 * probe, the self-test audit and the default-device selection exactly
 * the same way for every card.  Boot makes two calls and never changes
 * again: audiodev_init() at PCI time, audiodev_vfs_register() once the
 * device table exists.
 *
 * The streaming side is unified too.  Cards disagree about everything
 * below the sample (AC'97 wants a Buffer Descriptor List over I/O ports,
 * HDA wants a codec-wired DMA stream in memory space) but they agree on
 * the currency: signed 16-bit stereo frames.  audiodev_play() speaks that
 * currency to whichever card came up first, so mixer, tests and future
 * users of sound do not care which one it was.
 */
#ifndef GNUCOS_AUDIODEV_H
#define GNUCOS_AUDIODEV_H

#include <stdint.h>

struct audiodev;

/*
 * What a card offers.  probe and play are the only entries a playback-only
 * card must fill; everything else may be NULL and is skipped.
 */
struct audiodev_ops {
    const char *name;  /* subsys registry name: "ac97", "hda", ...   */
    uint16_t    major; /* Linux-ish major/minor for coldplug         */
    uint16_t    minor;

    /* Bring the card up.  1 on success, 0 if absent or unusable -- the
     * same convention ac97_init() and hda_init() already follow.        */
    int (*probe)(void);

    /* Optional audit run after a successful probe; 1 on pass.          */
    int (*selftest)(void);

    /* PCM-out: signed 16-bit stereo frames at the card's configured
     * rate.  Return 1 when the whole buffer was consumed.              */
    int (*play)(const int16_t *samples, uint32_t frames);

    /* Block until the engine has played everything queued.             */
    void (*drain)(void);

    /*
     * Engine accounting for the calsa (compat ALSA) layer.  All optional:
     * a card that fills them is routable through /dev/snd's pcm node, a
     * card that leaves them NULL is still registered, audited and usable
     * through audiodev_play().  These mirror the primitives audio.c's
     * AC'97 engine already provides, which is where the shape comes from.
     */
    uint32_t (*frames_total)(void); /* frames ever handed to the engine */
    uint32_t (*in_flight)(void);    /* queued, not yet played           */
    int (*free_frames)(void);       /* ring space left, in frames       */
    int (*engine_halted)(void);     /* engine stopped under us (XRUN)   */
    void (*reset_ring)(void);       /* rewind the descriptor ring       */
    void (*begin)(void);            /* start/resume the DMA engine      */
    void (*stop)(void);             /* halt the DMA engine              */
};

/* One registered card.  Public only so introspection can read it. */
struct audiodev {
    const struct audiodev_ops *ops;
    int                        slot;  /* subsys registry slot, -1 before register        */
    uint8_t                    state; /* SUBSYS_STATE_* once probe has run               */
};

/*
 * The one registration function.  Announces the card into the subsys
 * registry, runs its probe, flips the slot to LIVE or FAILED, runs the
 * self-test on success and remembers the first playable card as the
 * default output.  Returns the audiodev slot, or -1 if the table is
 * full or the name is already taken.  Safe to call any time after
 * subsys_init(); audiodev_init() uses it for the built-in list.
 */
int audiodev_register(const struct audiodev_ops *ops);

/*
 * Boot's single call at PCI time: registers and probes every built-in
 * card driver, in table order.  No caller ever names ac97 or hda.
 */
int audiodev_init(void);

/*
 * Boot's single call after vfs_init(): publish the /dev nodes the audio
 * stack owns (/dev/dsp and the /dev/snd pair).  Deferred because the
 * device table does not exist until the root filesystem is mounted.
 */
int audiodev_vfs_register(void);

/* The first registered card that can play, or NULL. */
struct audiodev *audiodev_default(void);

/* How many cards were registered, and read-only access to one of them. */
int                    audiodev_count(void);
const struct audiodev *audiodev_get(int idx);

/* Unified streaming: route to the default card.  1 on full consumption. */
int  audiodev_play(const int16_t *samples, uint32_t frames);
void audiodev_drain(void);

/*
 * calsa (compat ALSA) engine hooks: the /dev/snd frontend talks to these
 * instead of any card's engine.  Each is routed to the default card and
 * returns 0 / "nothing happening" when that card leaves the hook NULL.
 */
uint32_t audiodev_pcm_frames_total(void);
uint32_t audiodev_pcm_in_flight(void);
int      audiodev_pcm_free_frames(void);
int      audiodev_pcm_halted(void);
int      audiodev_pcm_reset(void);
int      audiodev_pcm_begin(void);
int      audiodev_pcm_stop(void);

/* Default card's name ("ac97", "hda", ...) for user-visible strings;
 * "none" when nothing playable registered. */
const char *audiodev_default_name(void);

#endif /* GNUCOS_AUDIODEV_H */

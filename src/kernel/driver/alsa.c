/*
 * alsa.c — the ALSA kernel ABI over the AC97 engine: /dev/snd/*. (GPLv2)
 *
 * Implements enough of the ALSA UAPI for libasound's "hw" device path:
 *
 *   /dev/snd/controlC0   card info + PCM info (enumeration)
 *   /dev/snd/pcmC0D0p    playback: HW_REFINE, HW_PARAMS, SW_PARAMS,
 *                        PREPARE, START, WRITEI_FRAMES, SYNC_PTR, DRAIN,
 *                        DROP, PAUSE, RESET
 *   /dev/snd/pcmC0D0c    capture stub (returns no data)
 *   /dev/snd/timer       stub
 *
 * The hardware is fixed: S16_LE, 2 channels, 48 kHz, 1024-frame periods,
 * 16 periods.  HW_REFINE converges every mask/interval onto exactly that
 * setup, which is all libasound needs to drive the hw plugin (its plug
 * layer converts anything else in user space).
 */
#include <stdint.h>

#include "audio.h"
#include "alsa.h"
#include "kstring.h"
#include "vfs.h"
#include "sound/asound.h"
#include "sysnum.h"
#include "vmm.h"
#include "pmm.h"

extern uint64_t g_hhdm;

#define ALSA_CARD        0
#define ALSA_RATE        48000
#define ALSA_CHANNELS    2
#define ALSA_PERIOD_FRAMES 1024
#define ALSA_PERIODS     16
#define ALSA_FORMAT      SNDRV_PCM_FORMAT_S16_LE

static int g_ctl_version = SNDRV_CTL_VERSION;   /* from the vendored header */

/* per-fd PCM state (a process has one pcm fd per stream) */
typedef struct {
    int      state;                /* SNDRV_PCM_STATE_* */
    uint32_t appl;                 /* frames written since prepare */
    uint32_t hw;                   /* frames consumed by the DMA */
    uint32_t start_threshold;
    uint32_t avail_min;
    uint32_t period_frames;
    uint32_t periods;
} pcm_stream_t;

/* ============================ helpers ==================================== */

static void mask_one(struct snd_mask *m, unsigned bit)
{
    memset(m->bits, 0, sizeof m->bits);
    m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void iv_set(struct snd_interval *i, unsigned v)
{
    i->min = i->max = v;
    i->integer = 1;
    i->openmin = i->openmax = i->empty = 0;
}

static void iv_range(struct snd_interval *i, unsigned lo, unsigned hi)
{
    i->min = lo; i->max = hi;
    i->integer = 1;
    i->openmin = i->openmax = i->empty = 0;
}

/* Converge one interval parameter onto [v, v].  Returns 1 if the user's
 * interval changed (cmask bookkeeping), 0 if it already matched, and marks
 * the whole params empty when the user's range excludes our value. */
static int iv_refine(struct snd_interval *i, unsigned v)
{
    if (i->empty)
        return 0;
    if (v < i->min || v > i->max)
        i->empty = 1;
    int changed = (i->min != v) || (i->max != v) || i->openmin || i->openmax;
    iv_set(i, v);
    return changed;
}

static int iv_refine_range(struct snd_interval *i, unsigned lo, unsigned hi)
{
    if (i->empty)
        return 0;
    if (i->max < lo || i->min > hi)
        i->empty = 1;
    int changed = 0;
    if (i->min < lo) { i->min = lo; changed = 1; }
    if (i->max > hi) { i->max = hi; changed = 1; }
    if (i->min > i->max)
        i->empty = 1;
    return changed;
}

static struct snd_interval *pcm_iv(struct snd_pcm_hw_params *p, unsigned param)
{
    return &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* ============================ controlC0 ================================== */

static int32_t ctl_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    (void)n;
    switch (cmd) {
    case SNDRV_CTL_IOCTL_PVERSION: {
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        *(int32_t *)(uintptr_t)arg = (int32_t)g_ctl_version;
        return 0;
    }
    case SNDRV_CTL_IOCTL_CARD_INFO: {
        if (!user_ptr_ok(arg, sizeof(struct snd_ctl_card_info)))
            return -E_FAULT;
        struct snd_ctl_card_info *info = (void *)(uintptr_t)arg;
        memset(info, 0, sizeof *info);
        info->card = ALSA_CARD;
        info->pad = 0;
        info->id[0] = 'G'; info->id[1] = 'N'; info->id[2] = 'O'; info->id[3] = 'S';
        info->id[4] = 0;
        strncpy(info->driver, "gnos-ac97", sizeof info->driver - 1);
        strncpy(info->name, "GNOS AC97", sizeof info->name - 1);
strncpy(info->longname, "GNOS Intel 82801AA AC97", sizeof info->longname - 1);
        strncpy(info->mixername, "GNOS AC97", sizeof info->mixername - 1);
        info->components[0] = 0;
        return 0;
    }
    case SNDRV_CTL_IOCTL_PCM_INFO: {
        if (!user_ptr_ok(arg, sizeof(struct snd_pcm_info)))
            return -E_FAULT;
        struct snd_pcm_info *info = (void *)(uintptr_t)arg;
        unsigned dev = info->device;
        memset(info, 0, sizeof *info);
        info->device = dev;
        info->subdevice = 0;
        info->stream = info->stream;    /* caller preset: 0 play, 1 capture */
        info->card = ALSA_CARD;
        if (info->stream == 0)
            strncpy(info->name, "GNOS AC97 PCM out", sizeof info->name - 1);
        else
            strncpy(info->name, "GNOS AC97 PCM in", sizeof info->name - 1);
        info->subdevices_count = 1;
        return 0;
    }
    case SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE: {
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        int32_t *dev = (int32_t *)(uintptr_t)arg;
        if (*dev < 0)
            *dev = 0;
        else
            *dev = 1;                   /* one PCM device, then done */
        return 0;
    }
    case SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE:
        return 0;
    case SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS:
        return 0;
    case SNDRV_CTL_IOCTL_ELEM_LIST: {
        if (!user_ptr_ok(arg, sizeof(struct snd_ctl_elem_list)))
            return -E_FAULT;
        struct snd_ctl_elem_list *l = (void *)(uintptr_t)arg;
        l->count = 0;                   /* no mixer elements yet */
        l->used = 0;
        return 0;
    }
    default:
        return -E_NOTTY;
    }
}

/* ============================ pcmC0D0p =================================== */

static pcm_stream_t g_play;            /* single stream (single app) */

/* The mmap status/control pages libasound REQUIRES: the hw plugin mmaps
 * them at SNDRV_PCM_MMAP_OFFSET_STATUS/CONTROL and reads hw_ptr directly.
 * Without them the open fails with ENOTTY ("audio open error"). */
static uint64_t g_status_phys, g_control_phys;
static struct snd_pcm_mmap_status  *g_status_k;
static struct snd_pcm_mmap_control *g_control_k;

static void pcm_sync_status(void)
{
    if (!g_status_k)
        return;
    g_status_k->state = g_play.state;
    g_status_k->hw_ptr = g_play.hw;
}

static uint32_t pcm_hw_frames(void)
{
    uint32_t hw = audio_frames_total() - audio_in_flight_frames();
    /* the ALSA ring wraps at period*periods frames */
    uint32_t boundary = ALSA_PERIOD_FRAMES * ALSA_PERIODS;
    return hw % boundary;
}

static int32_t alsa_pcm_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    (void)n;
    switch (cmd) {
    case SNDRV_PCM_IOCTL_INFO: {
        if (!user_ptr_ok(arg, sizeof(struct snd_pcm_info)))
            return -E_FAULT;
        struct snd_pcm_info *info = (void *)(uintptr_t)arg;
        memset(info, 0, sizeof *info);
        info->stream = 0;
        info->subdevices_count = 1;
        strncpy(info->name, "GNOS AC97 PCM out", sizeof info->name - 1);
        return 0;
    }
    case SNDRV_PCM_IOCTL_HW_REFINE:
    case SNDRV_PCM_IOCTL_HW_PARAMS: {
        if (!user_ptr_ok(arg, sizeof(struct snd_pcm_hw_params)))
            return -E_FAULT;
        struct snd_pcm_hw_params *p = (void *)(uintptr_t)arg;
        uint32_t cmask = 0;

        if (p->masks[0].bits[0] & 1u) {          /* ACCESS: keep interleaved */
            mask_one(&p->masks[0], 0);           /* SNDRV_PCM_ACCESS_RW_INTERLEAVED */
        } else {
            mask_one(&p->masks[0], 0);
            cmask |= 1u << 0;
        }
        /* FORMAT: S16_LE (index 2) */
        {
            struct snd_mask want;
            mask_one(&want, ALSA_FORMAT);
            struct snd_mask u = p->masks[1];
            int overlap = 0;
            for (int i = 0; i < 8; i++)
                overlap |= (u.bits[i] & want.bits[i]);
            mask_one(&p->masks[1], ALSA_FORMAT);
            if (!overlap)
                p->masks[1].bits[0] = 0;         /* empty mask: no match */
            cmask |= 1u << 1;
        }
        /* SUBFORMAT: STD (0) */
        mask_one(&p->masks[2], 0);
        cmask |= 1u << 2;

        if (iv_refine(pcm_iv(p, 8), 16))           /* SAMPLE_BITS  */
            cmask |= 1u << 8;
        if (iv_refine(pcm_iv(p, 9), 32))           /* FRAME_BITS   */
            cmask |= 1u << 9;
        if (iv_refine(pcm_iv(p, 10), ALSA_CHANNELS))  /* CHANNELS */
            cmask |= 1u << 10;
        if (iv_refine(pcm_iv(p, 11), ALSA_RATE))   /* RATE */
            cmask |= 1u << 11;
        if (iv_refine(pcm_iv(p, 12),
                      ALSA_PERIOD_FRAMES * 1000000u / ALSA_RATE))  /* PERIOD_TIME */
            cmask |= 1u << 12;
        if (iv_refine(pcm_iv(p, 13), ALSA_PERIOD_FRAMES))  /* PERIOD_SIZE */
            cmask |= 1u << 13;
        if (iv_refine_range(pcm_iv(p, 14),
                            ALSA_PERIOD_FRAMES * ALSA_CHANNELS * 2,
                            ALSA_PERIOD_FRAMES * ALSA_CHANNELS * 2))  /* PERIOD_BYTES */
            cmask |= 1u << 14;
        if (iv_refine(pcm_iv(p, 15), ALSA_PERIODS))  /* PERIODS */
            cmask |= 1u << 15;
        if (iv_refine(pcm_iv(p, 16),
                      ALSA_PERIOD_FRAMES * 1000000u / ALSA_RATE * ALSA_PERIODS))  /* BUFFER_TIME */
            cmask |= 1u << 16;
        if (iv_refine(pcm_iv(p, 17), ALSA_PERIOD_FRAMES * ALSA_PERIODS))  /* BUFFER_SIZE */
            cmask |= 1u << 17;
        if (iv_refine_range(pcm_iv(p, 18),
                            ALSA_PERIOD_FRAMES * ALSA_PERIODS * ALSA_CHANNELS * 2,
                            ALSA_PERIOD_FRAMES * ALSA_PERIODS * ALSA_CHANNELS * 2))  /* BUFFER_BYTES */
            cmask |= 1u << 18;

        p->info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER;
        p->msbits = 16;
        p->rate_num = ALSA_RATE;
        p->rate_den = 1;
        p->fifo_size = 0;
        p->cmask |= cmask;
        return 0;
    }
    case SNDRV_PCM_IOCTL_SW_PARAMS: {
        if (!user_ptr_ok(arg, sizeof(struct snd_pcm_sw_params)))
            return -E_FAULT;
        struct snd_pcm_sw_params *sw = (void *)(uintptr_t)arg;
        g_play.start_threshold = (uint32_t)sw->start_threshold;
        g_play.avail_min = (uint32_t)sw->avail_min;
        return 0;
    }
    case SNDRV_PCM_IOCTL_STATUS:
    case SNDRV_PCM_IOCTL_STATUS_EXT: {
        if (!user_ptr_ok(arg, sizeof(struct snd_pcm_status)))
            return -E_FAULT;
        struct snd_pcm_status *st = (void *)(uintptr_t)arg;
        memset(st, 0, sizeof *st);
        st->state = g_play.state;
        st->appl_ptr = g_play.appl;
        st->hw_ptr = g_play.hw;
        return 0;
    }
    case SNDRV_PCM_IOCTL_PREPARE:
        audio_reset_ring();
        g_play.appl = 0;
        g_play.hw = 0;
        g_play.state = SNDRV_PCM_STATE_PREPARED;
        pcm_sync_status();
        return 0;
    case SNDRV_PCM_IOCTL_RESET:
        audio_reset_ring();
        g_play.appl = 0;
        g_play.hw = 0;
        g_play.state = SNDRV_PCM_STATE_SETUP;
        pcm_sync_status();
        return 0;
    case SNDRV_PCM_IOCTL_START:
        if (g_play.state != SNDRV_PCM_STATE_PREPARED)
            return -E_BADF;
        audio_begin();
        g_play.state = SNDRV_PCM_STATE_RUNNING;
        pcm_sync_status();
        return 0;
    case SNDRV_PCM_IOCTL_DROP:
        audio_stop();
        g_play.state = SNDRV_PCM_STATE_SETUP;
        pcm_sync_status();
        return 0;
    case SNDRV_PCM_IOCTL_DRAIN:
        if (g_play.state == SNDRV_PCM_STATE_RUNNING) {
            audio_drain();
        }
        g_play.state = SNDRV_PCM_STATE_SETUP;
        pcm_sync_status();
        return 0;
    case SNDRV_PCM_IOCTL_PAUSE: {
        int32_t on = (int32_t)arg;
        if (on)
            audio_stop();
        else if (g_play.state == SNDRV_PCM_STATE_PAUSED ||
                 g_play.state == SNDRV_PCM_STATE_RUNNING)
            audio_begin();
        g_play.state = on ? SNDRV_PCM_STATE_PAUSED : SNDRV_PCM_STATE_RUNNING;
        pcm_sync_status();
        return 0;
    }
    case SNDRV_PCM_IOCTL_WRITEI_FRAMES: {
        if (!user_ptr_ok(arg, sizeof(struct snd_xferi)))
            return -E_FAULT;
        struct snd_xferi *x = (void *)(uintptr_t)arg;
        if (!user_ptr_ok((uint64_t)(uintptr_t)x->buf,
                         (uint32_t)x->frames * ALSA_CHANNELS * 2))
            return -E_FAULT;
        int fed = audio_write((const int16_t *)(uintptr_t)x->buf,
                              (uint32_t)x->frames);
        if (fed < 0)
            return -E_IO;
        g_play.appl += (uint32_t)fed;
        if (g_play.state == SNDRV_PCM_STATE_PREPARED &&
            g_play.appl >= g_play.start_threshold) {
            audio_begin();
            g_play.state = SNDRV_PCM_STATE_RUNNING;
        }
        if (audio_engine_halted() && g_play.state == SNDRV_PCM_STATE_RUNNING)
            g_play.state = SNDRV_PCM_STATE_XRUN;
        x->result = fed;
        pcm_sync_status();
        return 0;
    }
    case __SNDRV_PCM_IOCTL_SYNC_PTR: {
        if (!user_ptr_ok(arg, sizeof(struct snd_pcm_sync_ptr)))
            return -E_FAULT;
        struct snd_pcm_sync_ptr *sp = (void *)(uintptr_t)arg;
        if (sp->flags & SNDRV_PCM_SYNC_PTR_APPL)
            g_play.appl = (uint32_t)sp->c.control.appl_ptr;
        if (audio_engine_halted() && g_play.state == SNDRV_PCM_STATE_RUNNING)
            g_play.state = SNDRV_PCM_STATE_XRUN;
        sp->s.status.state = g_play.state;
        sp->s.status.hw_ptr = g_play.hw;
        sp->c.control.appl_ptr = g_play.appl;
        sp->c.control.avail_min = g_play.avail_min;
        pcm_sync_status();
        return 0;
    }
    case SNDRV_PCM_IOCTL_TSTAMP:
        return 0;
    case SNDRV_PCM_IOCTL_HW_FREE:
        return 0;
    case SNDRV_PCM_IOCTL_READI_FRAMES:
        return -E_NOTTY;                 /* capture not implemented */
    default:
        dbg_puts("ALSA: unknown ctl cmd=");
        dbg_puts_hex(cmd);
        dbg_puts("\r\n");
        return -E_NOTTY;
    }
}

/* ============================ device glue ================================ */

static pcm_stream_t *pcm_stream_of(vfs_node_t *n)
{
    (void)n;
    return &g_play;
}

static int32_t pcm_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return 0;                                   /* capture not implemented */
}

static int32_t pcm_write(vfs_node_t *n, uint64_t off, const void *buf, uint32_t len)
{
    (void)pcm_stream_of(n); (void)off; (void)buf; (void)len;
    return 0;                                   /* transfers go through ioctls */
}

static int32_t pcm_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    return alsa_pcm_ioctl(n, cmd, arg);
}

static int pcm_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    (void)n;
    int16_t r = 0;
    if ((events & POLLOUT) && audio_free_frames() > 0)
        r |= POLLOUT;
    if (events & POLLIN)
        r |= 0;
    *revents = r;
    return 0;
}

static int32_t ctl_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return 0;
}

static int32_t ctl_ioctl_w(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    return ctl_ioctl(n, cmd, arg);
}

static const vfs_ops_t g_ctl_ops = {
    .read  = ctl_read,
    .ioctl = ctl_ioctl_w,
};

static int pcm_mmap(vfs_node_t *n, uint64_t offset, uint64_t *phys,
                    uint64_t *size)
{
    (void)n;
    if (offset == SNDRV_PCM_MMAP_OFFSET_STATUS_OLD ||
        offset == SNDRV_PCM_MMAP_OFFSET_STATUS_NEW) {
        *phys = g_status_phys;
        *size = 4096;
        return 0;
    }
    if (offset == SNDRV_PCM_MMAP_OFFSET_CONTROL_OLD ||
        offset == SNDRV_PCM_MMAP_OFFSET_CONTROL_NEW) {
        *phys = g_control_phys;
        *size = 4096;
        return 0;
    }
    return -E_INVAL;
}

static const vfs_ops_t g_pcm_ops = {
    .read  = pcm_read,
    .write = pcm_write,
    .ioctl = pcm_ioctl,
    .poll  = pcm_poll,
    .mmap  = pcm_mmap,
};

int alsa_vfs_register(void)
{
    g_status_phys = pmm_alloc_zeroed();
    g_control_phys = pmm_alloc_zeroed();
    if (!g_status_phys || !g_control_phys)
        return -E_NOMEM;
    g_status_k = (struct snd_pcm_mmap_status *)(uintptr_t)(g_status_phys + g_hhdm);
    g_control_k = (struct snd_pcm_mmap_control *)(uintptr_t)(g_control_phys + g_hhdm);
    g_status_k->state = SNDRV_PCM_STATE_OPEN;
    {
    vfs_register_devnum("snd/controlC0", &g_ctl_ops, NULL, 116, 0);
    vfs_register_devnum("snd/pcmC0D0p", &g_pcm_ops, NULL, 116, 16);
    vfs_register_devnum("snd/pcmC0D0c", &g_pcm_ops, NULL, 116, 24);
    vfs_register_devnum("snd/timer", &g_ctl_ops, NULL, 116, 33);
    g_play.state = SNDRV_PCM_STATE_OPEN;
    g_play.start_threshold = 1;
    g_play.avail_min = ALSA_PERIOD_FRAMES;
    }
    pcm_sync_status();
    return 0;
}

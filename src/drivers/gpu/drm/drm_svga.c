/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_svga.c — the VMware SVGA II register interface and the scanout target
 * it owns. (GPLv2)
 *
 * The device is reached the way drm_vbe.c reaches VBE — through the PCI scan
 * — but the two are otherwise unrelated.  VBE is a 16-bit register bank that
 * reshapes a framebuffer someone else owns; SVGA II is a 32-bit register bank
 * backed by a framebuffer of its own (BAR1) plus a command FIFO (BAR2).  We
 * program a mode, enable the device, and from then on the DRM refresh path
 * copies into SVGA VRAM instead of into the console framebuffer.
 *
 * Two details are worth knowing before reading further:
 *
 *  - The registers come in two flavours.  BAR0 is either memory-mapped (then
 *    register `n` is the 32-bit word at index n) or an I/O index/value pair
 *    (write the index to base+0, then read or write base+4).  svga_read()
 *    and svga_write() hide which one we got; every access is 32 bits wide.
 *  - The mode is the one fbcon already runs, not one we invent.  The KMS
 *    connector advertises that mode and the copy loop clips to it, so
 *    picking a different geometry here would desynchronise the two.  MAX_*
 *    and FB_SIZE are only used to clamp and to reject a mode the device
 *    cannot hold.
 */

#include <stddef.h>
#include <stdint.h>

#include "debugcon.h"
#include "drm_init.h"
#include "drm_svga.h"
#include "fbcon.h"
#include "io.h"
#include "pci.h"
#include "pmm.h"
#include "proc.h"
#include "vmm.h"

/* The guest buffer we accelerate out of lives in the direct map, so its
 * physical address is a subtraction rather than a page-table walk. */
extern uint64_t g_hhdm;

#define PCI_VENDOR_VMWARE 0x15AD
#define PCI_DEVICE_SVGA2  0x0405 /* "SVGA II": the only one we drive */

/*
 * Offsets of the two ports inside BAR0 when it is an I/O BAR.  They are not
 * four bytes apart: the value port sits one byte above the index port, which
 * is what the device answers on (measured: writing SVGA_ID_2 through
 * base+1 and reading it back returns it, base+4 reads as 0xFFFFFFFF, and
 * base+2 answers 0xCAFE, the BIOS port).  Both are read and written with
 * 32-bit port accesses.
 */
#define SVGA_PORT_INDEX 0
#define SVGA_PORT_VALUE 1

/* Register indices.  Each names one 32-bit register, not a byte offset. */
#define SVGA_REG_ID                  0
#define SVGA_REG_ENABLE              1
#define SVGA_REG_WIDTH               2
#define SVGA_REG_HEIGHT              3
#define SVGA_REG_MAX_WIDTH           4
#define SVGA_REG_MAX_HEIGHT          5
#define SVGA_REG_BITS_PER_PIXEL      7
#define SVGA_REG_BYTES_PER_LINE      12
#define SVGA_REG_FB_START            13
#define SVGA_REG_FB_OFFSET           14
#define SVGA_REG_VRAM_SIZE           15
#define SVGA_REG_FB_SIZE             16
#define SVGA_REG_CAPABILITIES        17
#define SVGA_REG_MEM_START           18
#define SVGA_REG_MEM_SIZE            19
#define SVGA_REG_CONFIG_DONE         20
#define SVGA_REG_SYNC                21
#define SVGA_REG_BUSY                22
#define SVGA_REG_HOST_BITS_PER_PIXEL 28
#define SVGA_REG_MEM_REGS            30

/* Cursor registers. */
#define SVGA_REG_CURSOR_ID 24
#define SVGA_REG_CURSOR_ON 27

/* What SVGA II answers on SVGA_REG_ID once it has been woken up. */
#define SVGA_ID_2 0x90000002u

/* The command FIFO starts with a small header of 32-bit words. */
#define SVGA_FIFO_MIN      0
#define SVGA_FIFO_MAX      1
#define SVGA_FIFO_NEXT_CMD 2
#define SVGA_FIFO_STOP     3

/* The device writes here the value of the newest fence it has finished.
 * Completion is decided by reading this back, which is the one part of the
 * fence protocol that is easy to verify: emit N, sync, expect >= N. */
#define SVGA_FIFO_FENCE 6

/* FIFO commands.  Only the ones this driver issues. */
#define SVGA_CMD_UPDATE               1  /* (x, y, w, h) */
#define SVGA_CMD_MOVE_CURSOR          21 /* (x, y) */
#define SVGA_CMD_DEFINE_ALPHA_CURSOR  22 /* (id, hotX, hotY, w, h, pixels...) */
#define SVGA_CMD_FENCE                30 /* (value) */
#define SVGA_CMD_DEFINE_GMRFB         36 /* (gmrId, offset, bytesPerLine) */
#define SVGA_CMD_BLIT_GMRFB_TO_SCREEN 37 /* (srcX, srcY, l, t, r, b) */
#define SVGA_CMD_DEFINE_GMR2          41 /* (gmrId, numPages) */
#define SVGA_CMD_REMAP_GMR2           42 /* (gmrId, numPages, pages...) */

/* The one GMR we ever bind: the guest buffer being scanned out. */
#define SVGA_GMR_ID 1

/* Alpha cursor images are at most 64x64 in this kernel (see
 * drm_mode_config.c), so a definition always fits in the FIFO. */
#define SVGA_CURSOR_MAX 64

/*
 * Whether the device's cursor overlay is allowed to replace the software
 * one.
 *
 * 0 until somebody has looked at a window and seen a cursor.  Handing the
 * cursor to hardware stops drm_dummy_draw_cursor() from painting, so if the
 * device silently ignores SVGA_CMD_DEFINE_ALPHA_CURSOR the machine ends up
 * with no pointer at all -- and that is not something a headless boot can
 * tell you.  The software cursor is the path that has actually been seen
 * working.  Flip to 1, run with a window, and leave it at 1 if the pointer
 * is there.
 */
#define SVGA_HW_CURSOR 0

/* vmm_map_mmio() turns away anything larger than this, so a framebuffer BAR
 * bigger than 16 MiB has to be mapped in pieces we actually need. */
#define VMM_MMIO_MAX (16ULL << 20)

/* How long to wait for SYNC to settle before giving up on a notification.
 * The device is never this slow; the bound is here so a wedged FIFO cannot
 * wedge the refresh thread with it. */
#define SVGA_SYNC_SPINS 1000000u

static const pci_dev_t   *g_dev;
static volatile uint32_t *g_mmio; /* BAR0, memory form */
static uint16_t           g_io;   /* BAR0, I/O index/value form */
static volatile uint32_t *g_fifo; /* BAR2, command FIFO */
static volatile uint32_t *g_vram; /* BAR1 + FB_OFFSET: the visible surface */
static uint32_t           g_w, g_h, g_pitch;
static int                g_present;

/* Fence values are issued from here, so "has fence N finished?" is a signed
 * comparison against SVGA_FIFO_FENCE and keeps working across the wrap. */
static uint32_t g_fence_next;

/* GMR state.  g_gmr_ok is only set by svga_selftest() once an accelerated
 * blit has been checked byte for byte; until then the copy path in
 * drm_init.c stays on the CPU. */
static int      g_gmr_ok;
static uint64_t g_gmr_phys; /* the run currently bound, so it is rebound
                             * only when the source buffer changes */
static uint64_t g_gmr_pages;
static int      g_gmr_ppn;   /* 1 = page descriptors are page numbers */
static int      g_gmr_bound; /* the above describes what is bound now */

/* Hardware cursor is showing, so drm_dummy_draw_cursor() must stop painting
 * one by hand. */
static int g_cursor_hw;

/* ---- register access --------------------------------------------------- */

static uint32_t svga_read(uint32_t reg)
{
    if (g_mmio != NULL) {
        return g_mmio[reg];
    }
    outl(g_io + SVGA_PORT_INDEX, reg);
    return inl(g_io + SVGA_PORT_VALUE);
}

static void svga_write(uint32_t reg, uint32_t val)
{
    if (g_mmio != NULL) {
        g_mmio[reg] = val;
        return;
    }
    outl(g_io + SVGA_PORT_INDEX, reg);
    outl(g_io + SVGA_PORT_VALUE, val);
}

/* ---- command FIFO ------------------------------------------------------ */

/*
 * Point the FIFO at itself.  MIN is where commands start (past the header
 * and past however many registers the device keeps there), MAX is its size,
 * and NEXT/STOP are left equal so the device sees an empty queue.  Done
 * before CONFIG_DONE, which is when the device latches the layout.
 */
static void svga_fifo_setup(void)
{
    uint32_t mem_size = svga_read(SVGA_REG_MEM_SIZE);
    uint32_t mem_regs = svga_read(SVGA_REG_MEM_REGS);
    uint64_t va;

    if (mem_size == 0) {
        return;
    }

    va = pci_map_bar(g_dev, 2);
    if (va == 0) {
        dbg_puts("drm: svga: no fifo bar, update notifications off\r\n");
        return;
    }
    g_fifo = (volatile uint32_t *)(uintptr_t)va;

    uint32_t min = mem_regs * 4;
    if (min < 16) {
        min = 16;
    }
    if (min >= mem_size) {
        return;
    }

    g_fifo[SVGA_FIFO_MIN]      = min;
    g_fifo[SVGA_FIFO_MAX]      = mem_size;
    g_fifo[SVGA_FIFO_NEXT_CMD] = min;
    g_fifo[SVGA_FIFO_STOP]     = min;

    dbg_puts("drm: svga: fifo min=");
    dbg_puts_dec(min);
    dbg_puts(" max=");
    dbg_puts_dec(mem_size);
    dbg_puts("\r\n");
}

/*
 * Claim @nwords of FIFO, or fail.
 *
 * The FIFO is a ring: commands live between MIN and MAX and the write
 * cursor wraps.  A command that will not fit before MAX goes at MIN
 * instead, which is only safe because the device wraps its own read cursor
 * at MAX too.  A command too big for the whole ring can never be issued.
 */
static volatile uint32_t *svga_fifo_reserve(uint32_t nwords)
{
    uint32_t max, min, next, bytes;

    if (g_fifo == NULL || nwords == 0) {
        return NULL;
    }

    min   = g_fifo[SVGA_FIFO_MIN];
    max   = g_fifo[SVGA_FIFO_MAX];
    bytes = nwords * 4;

    if (max <= min || bytes > max - min) {
        return NULL;
    }

    next = g_fifo[SVGA_FIFO_NEXT_CMD];
    if (next < min || next > max) {
        next = min;
    }
    if (next + bytes > max) {
        next = min;
    }

    g_fifo[SVGA_FIFO_NEXT_CMD] = next;
    return g_fifo + (next / 4);
}

/* Make what svga_fifo_reserve() handed out visible to the device. */
static void svga_fifo_commit(uint32_t nwords)
{
    uint32_t max, min, next;

    min  = g_fifo[SVGA_FIFO_MIN];
    max  = g_fifo[SVGA_FIFO_MAX];
    next = g_fifo[SVGA_FIFO_NEXT_CMD] + nwords * 4;

    if (next >= max) {
        next = min;
    }
    g_fifo[SVGA_FIFO_NEXT_CMD] = next;
}

/*
 * Push everything queued so far and wait for the device to be idle.  The
 * spin is bounded: a device that never answers must not take the refresh
 * thread down with it.
 */
static void svga_sync(void)
{
    svga_write(SVGA_REG_SYNC, 1);
    for (uint32_t i = 0; i < SVGA_SYNC_SPINS && svga_read(SVGA_REG_BUSY) != 0; i++) {
    }
}

/* ---- fences ------------------------------------------------------------ */

/*
 * Issue a fence and return its value, or 0 if it could not be issued.  A
 * fence is how a driver learns that a blit has landed without stalling the
 * CPU on SYNC: emit it after the commands, then poll SVGA_FIFO_FENCE.
 */
static uint32_t svga_fence_emit(void)
{
    volatile uint32_t *cmd = svga_fifo_reserve(2);
    uint32_t           f;

    if (cmd == NULL) {
        return 0;
    }

    f      = ++g_fence_next;
    cmd[0] = SVGA_CMD_FENCE;
    cmd[1] = f;
    svga_fifo_commit(2);
    svga_sync();
    return f;
}

/* Has the device reached fence @f?  Signed so the wrap compares correctly. */
static int svga_fence_passed(uint32_t f)
{
    if (f == 0 || g_fifo == NULL) {
        return 0;
    }
    return (int32_t)(g_fifo[SVGA_FIFO_FENCE] - f) >= 0;
}

void svga_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    volatile uint32_t *cmd;

    if (!g_present || w == 0 || h == 0) {
        return;
    }

    cmd = svga_fifo_reserve(5);
    if (cmd == NULL) {
        return;
    }

    cmd[0] = SVGA_CMD_UPDATE;
    cmd[1] = x;
    cmd[2] = y;
    cmd[3] = w;
    cmd[4] = h;
    svga_fifo_commit(5);

    svga_sync();
}

/* ---- hardware cursor --------------------------------------------------- */

/*
 * Hand the device the cursor image itself rather than painting one over the
 * scanout after every copy.  The alpha form takes ARGB pixels directly, so
 * the buffer a client gave us needs no AND/XOR mask conversion — the only
 * thing to watch is channel order, which is a matter of looking at the
 * screen rather than of anything the registers will tell us.
 */
int svga_cursor_define(uint32_t id, uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y,
                       const uint32_t *argb)
{
    volatile uint32_t *cmd;
    uint32_t           npix = w * h;

    if (!g_present || argb == NULL) {
        return -EINVAL;
    }
    if (w == 0 || h == 0 || w > SVGA_CURSOR_MAX || h > SVGA_CURSOR_MAX) {
        return -EINVAL;
    }

    cmd = svga_fifo_reserve(6 + npix);
    if (cmd == NULL) {
        return -EBUSY;
    }

    cmd[0] = SVGA_CMD_DEFINE_ALPHA_CURSOR;
    cmd[1] = id;
    cmd[2] = (uint32_t)hot_x;
    cmd[3] = (uint32_t)hot_y;
    cmd[4] = w;
    cmd[5] = h;
    for (uint32_t i = 0; i < npix; i++) {
        cmd[6 + i] = argb[i];
    }
    svga_fifo_commit(6 + npix);

    svga_sync();
    return 0;
}

void svga_cursor_move(int32_t x, int32_t y)
{
    volatile uint32_t *cmd;

    if (!g_present) {
        return;
    }

    cmd = svga_fifo_reserve(3);
    if (cmd == NULL) {
        return;
    }
    cmd[0] = SVGA_CMD_MOVE_CURSOR;
    cmd[1] = (uint32_t)x;
    cmd[2] = (uint32_t)y;
    svga_fifo_commit(3);
    svga_sync();
}

/* Turn the device's cursor overlay on or off.  From here on the software
 * cursor in drm_init.c would be a second, stale copy, so it stops drawing. */
void svga_cursor_show(int on)
{
    if (!g_present) {
        return;
    }

#if SVGA_HW_CURSOR
    if (on) {
        svga_write(SVGA_REG_CURSOR_ID, 1);
        svga_write(SVGA_REG_CURSOR_ON, 1);
    } else {
        svga_write(SVGA_REG_CURSOR_ON, 0);
    }
    g_cursor_hw = on ? 1 : 0;
#else
    /* Software cursor stays in charge; see SVGA_HW_CURSOR. */
    (void)on;
    g_cursor_hw = 0;
#endif
}

int svga_cursor_active(void)
{
    return g_cursor_hw;
}

/* ---- probe and mode set ------------------------------------------------ */

/*
 * Physical address behind a memory BAR, from the raw config words.  Only
 * needed for the fallback below: pci_map_bar() would compute this itself,
 * but it insists on mapping the whole BAR and vmm_map_mmio() will not take
 * more than 16 MiB.
 */
static uint64_t bar_phys(const pci_dev_t *d, int idx)
{
    uint32_t raw = d->bar[idx];

    if (raw == 0 || (raw & 0x1)) {
        return 0;
    }

    /* Type bits 2:1 — 2 means 64-bit, in which case the next BAR holds the
     * high half. */
    if ((raw & 0x6) == 0x4 && idx + 1 <= 5) {
        return ((uint64_t)d->bar[idx + 1] << 32) | (uint64_t)(raw & 0xFFFFFFF0u);
    }
    return (uint64_t)(raw & 0xFFFFFFF0u);
}

int svga_init(void)
{
    const pci_dev_t *d;
    uint64_t         vram_va;
    uint32_t         max_w, max_h, vram_sz, fb_sz;
    uint32_t         w, h, pitch, bpp, off, cap;

    d = pci_find(PCI_VENDOR_VMWARE, PCI_DEVICE_SVGA2);
    if (d == NULL) {
        return 0;
    }
    pci_enable(d);
    g_dev = d;

    /* BAR0: registers, whichever form this device exposes them in. */
    g_io = pci_bar_io(d, 0);
    if (g_io == 0) {
        uint64_t va = pci_map_bar(d, 0);
        if (va == 0) {
            return 0;
        }
        g_mmio = (volatile uint32_t *)(uintptr_t)va;
    }

    dbg_puts("drm: svga: ");
    dbg_puts(g_mmio != NULL ? "via PCI MMIO bar\r\n" : "via PCI ioports\r\n");

    /* Wake it up: SVGA II answers its own magic on the ID register. */
    svga_write(SVGA_REG_ID, SVGA_ID_2);
    if (svga_read(SVGA_REG_ID) != SVGA_ID_2) {
        dbg_puts("drm: svga: id mismatch, not SVGA II\r\n");
        return 0;
    }

    svga_write(SVGA_REG_ENABLE, 0);
    svga_write(SVGA_REG_CONFIG_DONE, 0);

    max_w   = svga_read(SVGA_REG_MAX_WIDTH);
    max_h   = svga_read(SVGA_REG_MAX_HEIGHT);
    vram_sz = svga_read(SVGA_REG_VRAM_SIZE);
    fb_sz   = svga_read(SVGA_REG_FB_SIZE);

    dbg_puts("drm: svga: max=");
    dbg_puts_dec(max_w);
    dbg_puts("x");
    dbg_puts_dec(max_h);
    dbg_puts(" vram=");
    dbg_puts_hex(vram_sz);
    dbg_puts(" fb=");
    dbg_puts_hex(fb_sz);
    dbg_puts(" hostbpp=");
    dbg_puts_dec(svga_read(SVGA_REG_HOST_BITS_PER_PIXEL));
    dbg_puts("\r\n");

    /* BAR1: the framebuffer.  A device with more VRAM than vmm_map_mmio()
     * will accept gets only the part a single screen needs. */
    vram_va = pci_map_bar(d, 1);
    if (vram_va == 0) {
        uint64_t phys = bar_phys(d, 1);
        uint64_t want = fb_sz != 0 ? fb_sz : vram_sz;
        if (phys == 0) {
            return 0;
        }
        if (want == 0 || want > VMM_MMIO_MAX) {
            want = VMM_MMIO_MAX;
        }
        vram_va = vmm_map_mmio(phys, want);
        dbg_puts("drm: svga: vram mapped in a capped window\r\n");
    }
    if (vram_va == 0) {
        dbg_puts("drm: svga: cannot framebuffer bar\r\n");
        return 0;
    }

    svga_fifo_setup();

    /* The mode is the one the console already has.  Clamp it to what the
     * device can do, then drop lines until the surface fits in the
     * framebuffer — a mode the device accepted but cannot hold would put
     * the copy loop past the end of VRAM. */
    fbcon_geometry(&w, &h, &pitch);
    if (w == 0 || h == 0) {
        return 0;
    }
    if (max_w != 0 && w > max_w) {
        w = max_w;
    }
    if (max_h != 0 && h > max_h) {
        h = max_h;
    }
    if (fb_sz != 0 && (uint64_t)w * 4u * h > fb_sz) {
        h = (uint32_t)(fb_sz / ((uint64_t)w * 4u));
    }
    if (h == 0) {
        return 0;
    }

    svga_write(SVGA_REG_WIDTH, w);
    svga_write(SVGA_REG_HEIGHT, h);
    svga_write(SVGA_REG_BITS_PER_PIXEL, 32);
    svga_write(SVGA_REG_CONFIG_DONE, 1);
    svga_write(SVGA_REG_ENABLE, 1);

    /* Read back what it actually took: the device is free to refuse a mode,
     * and copying into a surface that does not exist would be worse than
     * leaving the console framebuffer as the target. */
    w     = svga_read(SVGA_REG_WIDTH);
    h     = svga_read(SVGA_REG_HEIGHT);
    bpp   = svga_read(SVGA_REG_BITS_PER_PIXEL);
    pitch = svga_read(SVGA_REG_BYTES_PER_LINE);
    off   = svga_read(SVGA_REG_FB_OFFSET);

    if (bpp != 32 || pitch == 0 || w == 0 || h == 0) {
        dbg_puts("drm: svga: mode refused, keeping the console framebuffer\r\n");
        svga_write(SVGA_REG_ENABLE, 0);
        return 0;
    }

    cap = fb_sz != 0 ? fb_sz : vram_sz;
    if (vram_sz != 0 && cap > vram_sz) {
        cap = vram_sz;
    }
    if ((uint64_t)off + (uint64_t)pitch * h > cap) {
        dbg_puts("drm: svga: surface does not fit, keeping the console framebuffer\r\n");
        svga_write(SVGA_REG_ENABLE, 0);
        return 0;
    }

    g_vram    = (volatile uint32_t *)(uintptr_t)(vram_va + off);
    g_w       = w;
    g_h       = h;
    g_pitch   = pitch;
    g_present = 1;

    dbg_puts("drm: svga: mode ");
    dbg_puts_dec(g_w);
    dbg_puts("x");
    dbg_puts_dec(g_h);
    dbg_puts(" pitch=");
    dbg_puts_dec(g_pitch);
    dbg_puts(" fb_offset=");
    dbg_puts_hex(off);
    dbg_puts(" base=");
    dbg_puts_hex((uint64_t)(uintptr_t)g_vram);
    dbg_puts("\r\n");

    drm_scanout_set(g_vram, g_w, g_h, g_pitch);
    return 1;
}

/* ---- GMR: making a guest buffer reachable by the device ---------------- */

/*
 * Bind the run of pages at @phys into our one GMR.
 *
 * The page table is written inline into the FIFO, which is what makes this
 * the expensive part: a 1024x768x4 source is 768 pages, 6 KiB of command
 * per bind.  So a bind happens only when the source actually changes, not
 * once per frame.
 *
 * How a page is encoded is probed rather than assumed -- see svga_selftest().
 */
static int svga_gmr_bind(uint64_t phys, uint64_t bytes)
{
    uint64_t           pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t           nwords;
    volatile uint32_t *cmd;

    if (g_fifo == NULL || phys == 0 || pages == 0) {
        return -1;
    }

    nwords = 3u + (uint32_t)(pages * 2u);
    if ((uint64_t)nwords * 4u > g_fifo[SVGA_FIFO_MAX] - g_fifo[SVGA_FIFO_MIN]) {
        return -1;
    }

    if (g_gmr_phys == phys && g_gmr_pages == pages && g_gmr_bound) {
        return 0;
    }

    cmd = svga_fifo_reserve(3);
    if (cmd == NULL) {
        return -1;
    }
    cmd[0] = SVGA_CMD_DEFINE_GMR2;
    cmd[1] = SVGA_GMR_ID;
    cmd[2] = (uint32_t)pages;
    svga_fifo_commit(3);

    cmd = svga_fifo_reserve(nwords);
    if (cmd == NULL) {
        return -1;
    }
    cmd[0] = SVGA_CMD_REMAP_GMR2;
    cmd[1] = SVGA_GMR_ID;
    cmd[2] = (uint32_t)pages;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t page      = phys + i * PAGE_SIZE;
        uint64_t entry     = g_gmr_ppn ? (page >> 12) : page;
        cmd[3 + i * 2]     = (uint32_t)(entry & 0xFFFFFFFFu);
        cmd[3 + i * 2 + 1] = (uint32_t)(entry >> 32);
    }
    svga_fifo_commit(nwords);
    svga_sync();

    g_gmr_phys  = phys;
    g_gmr_pages = pages;
    g_gmr_bound = 1;
    return 0;
}

/* ---- GMRFB: the accelerated copy --------------------------------------- */

/*
 * The blit itself.  Deliberately not gated on g_gmr_ok: the self-test has to
 * be able to issue one before it has decided whether they work, and gating
 * it here turned the check into a chicken-and-egg that could only ever
 * report FAIL.  Callers outside this file get svga_blit(), which is gated.
 */
static int svga_gmrfb_issue(const void *src, uint32_t pitch, uint32_t w, uint32_t h,
                            uint32_t *fence_out)
{
    volatile uint32_t *cmd;
    uint64_t           phys;
    uint32_t           fence;

    if (!g_present || src == NULL) {
        return -1;
    }
    if (w == 0 || h == 0 || w > g_w || h > g_h || pitch == 0) {
        return -1;
    }
    if ((uint64_t)(uintptr_t)src < g_hhdm) {
        return -1;
    } /* not in the direct map */

    phys = (uint64_t)(uintptr_t)src - g_hhdm;
    if (svga_gmr_bind(phys, (uint64_t)pitch * h) != 0) {
        return -1;
    }

    cmd = svga_fifo_reserve(4);
    if (cmd == NULL) {
        return -1;
    }
    cmd[0] = SVGA_CMD_DEFINE_GMRFB;
    cmd[1] = SVGA_GMR_ID;
    cmd[2] = 0; /* offset within the GMR */
    cmd[3] = pitch;
    svga_fifo_commit(4);

    cmd = svga_fifo_reserve(7);
    if (cmd == NULL) {
        return -1;
    }
    cmd[0] = SVGA_CMD_BLIT_GMRFB_TO_SCREEN;
    cmd[1] = 0; /* srcOrigin.x */
    cmd[2] = 0; /* srcOrigin.y */
    cmd[3] = 0; /* destRect.left  */
    cmd[4] = 0; /* destRect.top   */
    cmd[5] = w; /* destRect.right */
    cmd[6] = h; /* destRect.bottom */
    svga_fifo_commit(7);

    /* A fence is always issued; a caller that hands one back intends to
     * poll it later (the refresh thread), one that does not wants the copy
     * finished by the time the call returns. */
    fence = svga_fence_emit();
    if (fence_out != NULL) {
        *fence_out = fence;
    } else {
        svga_sync();
    }
    return 0;
}

int svga_blit(const void *src, uint32_t pitch, uint32_t w, uint32_t h, uint32_t *fence_out)
{
    if (!g_gmr_ok) {
        return -1;
    } /* only after the self-test has proved it */
    return svga_gmrfb_issue(src, pitch, w, h, fence_out);
}

int svga_fence_wait(uint32_t fence, uint32_t max_ticks)
{
    for (uint32_t i = 0; i < max_ticks; i++) {
        if (svga_fence_passed(fence)) {
            return 0;
        }
        sched_block_timeout(WAIT_DRM, 1); /* doze a tick rather than spin */
    }
    return svga_fence_passed(fence) ? 0 : -1;
}

int svga_accelerated(void)
{
    return g_gmr_ok;
}

/* ---- self-test --------------------------------------------------------- */

/*
 * Decide, by measurement, whether the accelerated copy is usable at all.
 *
 * A small pattern is blitted and then read back out of the mapped VRAM and
 * compared byte for byte.  That comparison is also how the page-descriptor
 * encoding gets settled: try "physical address", try "page number", and
 * keep whichever one reproduces the source.  Until one does, svga_blit()
 * refuses to run and drm_init.c keeps its CPU copy.
 *
 * Measured on QEMU 8.2 (`-device vmware-svga`): the FIFO does run -- fences
 * come back -- but neither descriptor encoding reproduces the source, and a
 * screen->GMR blit leaves the source untouched, which says the binding in
 * DEFINE_GMR2/REMAP_GMR2 is what is not taking effect rather than the blit
 * arguments.  So on this host the answer is "no", the accelerated path
 * stays off, and the CPU copy stays.  Nothing here pretends otherwise.
 */
void svga_selftest(void)
{
    static const uint32_t tw = 64, th = 64;
    uint64_t              bytes = (uint64_t)tw * th * 4u;
    uint64_t              pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t              phys;
    uint32_t             *src;
    uint32_t              fence;
    int                   ok = 0;

    if (!g_present || g_vram == NULL) {
        return;
    }

    phys = pmm_alloc_contiguous(pages);
    if (phys == 0) {
        dbg_puts("SVGA: gmrfb blit SKIP (no test pages)\r\n");
        return;
    }
    src = (uint32_t *)pmm_virt(phys);

    /* A pattern no plausible bug would reproduce by accident. */
    for (uint32_t i = 0; i < tw * th; i++) {
        src[i] = 0xFF000000u | (i * 2654435761u);
    }

    for (int ppn = 0; ppn < 2 && !ok; ppn++) {
        g_gmr_ppn   = ppn;
        g_gmr_bound = 0;

        /* Wipe the target so a blit that does nothing cannot look like a
         * blit that worked. */
        for (uint32_t row = 0; row < th; row++) {
            for (uint32_t col = 0; col < tw; col++) {
                g_vram[(uint64_t)row * (g_pitch / 4) + col] = 0;
            }
        }

        if (svga_gmrfb_issue(src, tw * 4, tw, th, &fence) == 0 && svga_fence_wait(fence, 20) == 0) {
            ok = 1;
            for (uint32_t row = 0; row < th && ok; row++) {
                for (uint32_t col = 0; col < tw; col++) {
                    if (g_vram[(uint64_t)row * (g_pitch / 4) + col] !=
                        src[(uint64_t)row * tw + col]) {
                        ok = 0;
                        break;
                    }
                }
            }
        }

        dbg_puts("SVGA: gmrfb blit descriptor=");
        dbg_puts(ppn ? "pagenumber" : "physaddr");
        dbg_puts(ok ? " PASS\r\n" : " FAIL\r\n");
    }

    g_gmr_ok = ok;
    if (!ok) {
        g_gmr_ppn   = 0;
        g_gmr_bound = 0;
        dbg_puts("SVGA: gmrfb blit unusable, copy path stays on the CPU\r\n");
    } else {
        dbg_puts("SVGA: gmrfb blit enabled\r\n");
    }

    for (uint64_t i = 0; i < pages; i++) {
        pmm_free(phys + i * PAGE_SIZE);
    }
}

int svga_present(void)
{
    return g_present;
}

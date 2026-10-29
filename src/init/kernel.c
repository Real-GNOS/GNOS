/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kernel.c — GNOS 64-bit kernel (GNOSKr.elf), booted by Limine. (GPLv2)
 *
 * Limine enters kernel_entry() in long mode with paging already enabled:
 *   - the whole of physical RAM is direct-mapped at the HHDM offset
 *     (limine_hhdm_response.offset, normally 0xFFFF800000000000),
 *   - the kernel image lives at a per-boot randomized high-half base
 *     (kaslr.c moves it right after the handoff; see the kaslr section).
 * Every pointer Limine hands us (framebuffer address, module address, the
 * response structs themselves) is therefore an *already mapped* higher-half
 * virtual address -- we can dereference it straight away and must NOT try to
 * turn it back into a physical address.
 *
 * Bring-up order, and why:
 *   1. GDT/TSS and IDT first, so that from here on a bad memory access is a
 *      readable panic instead of a silent triple fault.
 *   2. Console, so the panic has somewhere to appear.
 *   3. PMM and VMM, because every later step allocates memory.
 *   4. VFS over the initrd, then the tty on top of it.
 *   5. The syscall gate and the timer.
 *   6. PID 1 (/sbin/init -> busybox init), and then the scheduler -- which never
 *      returns: this function becomes the idle loop.
 */
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "bootinfo.h"
#include "fbcon.h"
#include "debugcon.h"
#include "kaslr.h"
#include "vfs.h"
#include "cgroup.h"
#include "panic.h"
#include "gdt.h"
#include "idt.h"
#include "smp.h"
#include "tty.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "gfx.h"
#include "fbdev.h"
#include "input.h"
#include "subsys.h"
#include "sysfs.h"
#include "device.h"
#include "namespace.h"
#include "pty.h"
#include "audiodev.h"
#include "klog.h"
#include "cpuid.h"
#include "pagecache.h"
#include "slab.h"
#include "module.h"
#include "acpi_drv.h"
#include "../../crypto/crypto.h"
/* ACPICA entry points: declared by hand because including the vendored
 * acpi.h here would drag the platform selection into a file compiled
 * without ACPICA_CFLAGS.  ACPI_STATUS is UINT32. */
extern unsigned int AcpiInitializeSubsystem(void);
extern unsigned int AcpiInitializeTables(void *Table, unsigned int n, unsigned char f);
extern unsigned int AcpiLoadTables(void);
extern unsigned int AcpiEnableSubsystem(unsigned int Flags);
extern unsigned int AcpiInitializeObjects(unsigned int Flags);
#define ACPI_SUCCESS(st)         ((st) == 0)
#define ACPI_FULL_INITIALIZATION 0u
typedef unsigned int ACPI_STATUS_K;
#include "lapic.h"
#include "proc.h"
#include "timer.h"
#include "syscall.h"
#include "pci.h"
#include "e1000.h"
#include "rtl8139.h"
#include "rtl8169.h"
#include "nic.h"
#include "net.h"
#include "ata.h"
#include "nvme.h"
#include "drm.h"
#include "drm_init.h"
/* chrdev.h pulls drm_devtmpfs.h, which redeclares the kobject structs this
 * file already has -- declare the two init hooks instead of including it. */
void chrdev_init(void);
int  chrdev_populate(void);
#include "drm_svga.h"
#include "procfs.h"
#include "xhci.h"
#include "usb.h"
#include "netlink/netlink.h"
#include "gzip.h"
#include "usb_hid.h"

extern volatile struct limine_framebuffer_request        framebuffer_request;
extern volatile struct limine_module_request             module_request;
extern volatile struct limine_hhdm_request               hhdm_request;
extern volatile struct limine_kernel_address_request     kernel_address_request;
extern volatile struct limine_rsdp_request               rsdp_request;
extern volatile struct limine_executable_cmdline_request cmdline_request;

/* The boot command line (limine.conf's `cmdline:` line), kept for
 * /proc/cmdline; its words are also handed to PID 1 as argv[1..]. */
char g_boot_cmdline[256];

/* How often the scheduler pre-empts a running user process: SCHED_HZ, which
 * timer.h owns because times(2) and AT_CLKTCK have to report the same rate. */

/* bootinfo describing the framebuffer, shared with the console driver */
static bootinfo_t g_bi;

/* Physical/virtual geometry of the running kernel, filled in at entry. */
uint64_t g_hhdm;
uint64_t g_kernel_phys;
uint64_t g_kernel_virt;

/*
 * The Limine protocol entry point.  Deliberately thin: fill the globals the
 * rest of the boot needs, give KASLR its one chance to move the image, and
 * hand off to kernel_main().  The split matters -- if the kernel was
 * relocated, execution must never wander back into this function's old-base
 * instruction stream, or the randomization would be cosmetic.
 */
void kernel_main(void);

void kernel_entry(void)
{
    dbg_puts("GNOS: kernel_entry reached\r\n");

    /* ---- higher-half direct map offset ------------------------------- */
    g_hhdm = hhdm_request.response ? hhdm_request.response->offset : 0;
    dbg_puts("GNOS: hhdm   = ");
    dbg_puts_hex(g_hhdm);
    dbg_puts("\r\n");

    /* ---- where Limine actually put us -------------------------------- */
    if (kernel_address_request.response) {
        g_kernel_phys = kernel_address_request.response->physical_base;
        g_kernel_virt = kernel_address_request.response->virtual_base;
    }
    dbg_puts("GNOS: kphys  = ");
    dbg_puts_hex(g_kernel_phys);
    dbg_puts("\r\nGNOS: kvirt  = ");
    dbg_puts_hex(g_kernel_virt);
    dbg_puts("\r\n");

    /* ---- move the image to a random base, unless something says no ---- */
    uint64_t base = kaslr_maybe_relocate(g_kernel_virt, g_kernel_phys, g_hhdm);
    if (base != g_kernel_virt) {
        g_kernel_virt = base;
    }

    kernel_main();
}

/* Everything from the first framebuffer byte onward; runs at whatever base
 * kernel_entry settled on. */
void kernel_main(void)
{
    /* ---- framebuffer from Limine (HHDM virtual, already mapped) ------ */
    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        g_bi.fb_addr                  = (uint64_t)(uintptr_t)fb->address;
        g_bi.fb_width                 = (uint32_t)fb->width;
        g_bi.fb_height                = (uint32_t)fb->height;
        g_bi.fb_pitch                 = (uint32_t)fb->pitch;
        g_bi.fb_bpp                   = fb->bpp;
        g_bi.fb_type                  = GNUCOS_FB_RGB;
    }

    /* ---- initrd module from Limine (HHDM virtual too) ---------------- */
    /* Two ways to hold a root filesystem.  Live boot: a RAM module, which is
     * what the ISO ships.  Installed boot: nothing in RAM -- the root lives
     * in the first partition of the first ATA disk, written there by the
     * installer in user space.  The module wins when both exist.  The
     * disk-root branch is resolved later, once the allocator can give us
     * the 48 MiB a whole filesystem costs. */
    uint8_t *root_img  = NULL;
    uint32_t root_size = 0;
    int      initrd_gz = 0;
    if (module_request.response && module_request.response->module_count > 0) {
        struct limine_file *mod = module_request.response->modules[0];
        root_img                = (uint8_t *)(uintptr_t)mod->address;
        root_size               = (uint32_t)mod->size;
        /*
         * The initrd ships gzip-compressed (Makefile: initrd.img.gz on the
         * ISO), and Limine hands the module over verbatim.  Only sniff the
         * 1f 8b magic here: the inflate itself needs the PMM, which is not
         * up yet, so it runs after pmm_init() further down.
         */
        initrd_gz        = root_size > 18 && root_img[0] == 0x1f && root_img[1] == 0x8b;
        g_bi.initrd_addr = (uint64_t)(uintptr_t)mod->address;
        g_bi.initrd_size = mod->size;
        dbg_puts("GNOS: initrd = ");
        dbg_puts_hex(g_bi.initrd_addr);
        dbg_puts(" size=");
        dbg_puts_dec((uint32_t)g_bi.initrd_size);
        dbg_puts(initrd_gz ? " (gzip)\r\n" : "\r\n");
    } else {
        dbg_puts("GNOS: no initrd module; disk-root boot\r\n");
    }

    /* ---- CPU tables --------------------------------------------------- */
    gdt_init();
    idt_init();

    /* The driver registry has to exist before the first driver init runs,
     * because every one of them announces itself into it. */
    klog_init(); /* /dev-style kernel log ring */
    subsys_init();
    sysfs_init(); /* /sys built-in attributes */

    /* ---- console ------------------------------------------------------ */
    fbcon_init(&g_bi);
    fbcon_puts("GNOS (x86-64) booted via Limine\n");

    /* ---- memory ------------------------------------------------------- */
    pmm_init();
    vmm_init();
    vmm_map_kernel_bss(); /* bootloader may leave BSS unmapped */
    kheap_init();         /* kernel heap on top of the PMM */
    module_subsys_init(); /* loadable-module registry */

    /* Inflate the gzipped initrd now that the PMM is up: the decompressed
     * image is a contiguous run of frames -- far past the 16 MiB kernel
     * heap -- and it stays resident as the initrd the VFS mounts. */
    if (initrd_gz) {
        uint8_t *inflated      = NULL;
        size_t   inflated_size = 0;
        if (gzip_decompress(root_img, root_size, &inflated, &inflated_size) == 0) {
            root_img         = inflated;
            root_size        = (uint32_t)inflated_size;
            g_bi.initrd_addr = (uint64_t)(uintptr_t)inflated;
            g_bi.initrd_size = inflated_size;
            dbg_puts("GNOS: initrd inflated to ");
            dbg_puts_dec((uint32_t)g_bi.initrd_size);
            dbg_puts(" bytes\r\n");
        } else {
            dbg_puts("GNOS: initrd gzip decompress FAILED\r\n");
            g_bi.initrd_addr = 0;
            g_bi.initrd_size = 0;
            root_img         = NULL;
            root_size        = 0;
        }
    }

    /* ---- device model ---------------------------------------------------
     * kobject/sysfs tree plus the bus/driver/device registry.  Needs the
     * kernel heap (kcalloc for kobjects, kstrdup for names), hence after
     * kheap_init() and before any driver that registers devices. */
    device_model_init();

    /* The USB bus joins the model at boot so /sys/bus/usb exists before
     * the controller enumerates devices into it. */
    usb_sysfs_init();

    /* The seven namespaces (uts/ipc/mount/pid/net/user/cgroup) come up as
     * a set: init_nsproxy is static, so this only wires the bundle. */
    namespace_init();

    /* ---- firmware description ------------------------------------------
     * After the direct map is trustworthy (every ACPI pointer is physical and
     * gets read through it) and before the PCI probe, so that anything the
     * tables say about interrupt routing is already on hand. */
    acpi_init(rsdp_request.response ? (uint64_t)(uintptr_t)rsdp_request.response->address : 0);
    acpi_dump();
    acpi_pm1_init();

    /* ---- the ACPICA subsystem (interpreter, namespace, events) ---------
     * The table half above stays the boot-time source of MADT/FADT/HPET;
     * ACPICA adds the AML half: load every table, walk the namespace and
     * run _INI methods so devices the firmware hid behind _STA come out.
     * The OSL routes hardware access through the same primitives the rest
     * of the kernel uses. */
    {
        ACPI_STATUS_K st;
        st = AcpiInitializeSubsystem();
        dbg_puts("ACPI: InitializeSubsystem ");
        dbg_puts_dec((uint32_t)st);
        dbg_puts("\r\n");
        if (ACPI_SUCCESS(st)) {
            st = AcpiInitializeTables(NULL, 16, 0);
            dbg_puts("ACPI: InitializeTables ");
            dbg_puts_dec((uint32_t)st);
            dbg_puts("\r\n");
        }
        /* DISABLED while debugging: AcpiLoadTables trips a page fault
         * inside kmalloc (a heap header size field overwritten with a
         * small negative value -- some ACPICA path writes a negative
         * int32 past its allocation).  The subsystem stands at
         * InitializeTables until that is chased down with
         * ACPI_DBG_TRACK_ALLOCATIONS + gdb.
         * Live again for the gdb session that is chasing the heap
         * corruption (see commit history). */
        if (ACPI_SUCCESS(st)) {
            st = AcpiLoadTables();
            dbg_puts("ACPI: LoadTables ");
            dbg_puts_dec((uint32_t)st);
            dbg_puts("\r\n");
        }
        if (ACPI_SUCCESS(st)) {
            st = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
            dbg_puts("ACPI: EnableSubsystem ");
            dbg_puts_dec((uint32_t)st);
            dbg_puts("\r\n");
        }
        if (ACPI_SUCCESS(st)) {
            st = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
            dbg_puts("ACPI: InitializeObjects ");
            dbg_puts_dec((uint32_t)st);
            dbg_puts("\r\n");
        }
    }

    /* ---- SMP and the local APIC -----------------------------------------
     * After ACPI (the MADT is where each LAPIC's MMIO base and the APs'
     * lapic ids come from) and before the filesystem, so a wedged AP shows
     * up early.  The APs reach ap_main() and idle in the scheduler; the
     * BSP enables its own LAPIC here (the PIT stays its clock). */
    smp_init();
    lapic_init();

    /* ---- PCI devices --------------------------------------------------
     * After the allocators, because every driver here needs DMA buffers and
     * an uncacheable MMIO mapping, and before the filesystem, so that a card
     * that wedges the machine does so with the boot log still short enough
     * to read.  Each driver announces itself and then proves itself: the
     * NIC loops a frame through its own PHY, the codec streams a tone past
     * the DMA engine.  Neither test needs a human, a network or a speaker. */
    pci_init();

    /* The display device goes first: it claims the scanout target the DRM
     * pipeline will later size itself from, so it has to have answered
     * before drm_init_fallback() builds that pipeline.  Probing it here
     * also means "was there a GPU?" ends up in the registry like every
     * other device instead of being a line of boot text nobody kept.
     * FAILED is the ordinary case -- QEMU only has one with -device
     * vmware-svga -- and it costs nothing but a PCI lookup. */
    int slot_svga = subsys_register("svga", NULL, SUBSYS_CLASS_GRAPHIC, 0, 0);
    if (svga_init()) {
        subsys_set_state(slot_svga, SUBSYS_STATE_LIVE);
    } else {
        subsys_set_state(slot_svga, SUBSYS_STATE_FAILED);
    }

    /* Each probe reports into the registry so that "was there a NIC?" is a
     * lookup later on instead of a line of boot text nobody kept.  e1000 is
     * QEMU's default card and rtl8139 is the other classic; whichever binds
     * first owns the nic_* slot the IP stack talks to (see drivers/net/nic.h). */
    int nic_ok = e1000_init();
    if (!nic_ok)
        nic_ok = rtl8169_init();
    if (!nic_ok)
        nic_ok = rtl8139_init();
    int slot_nic = subsys_register(nic_ok ? nic_name() : "net", NULL, SUBSYS_CLASS_NET, 0, 0);
    if (nic_ok) {
        subsys_set_state(slot_nic, SUBSYS_STATE_LIVE);
        nic_selftest();
    } else {
        subsys_set_state(slot_nic, SUBSYS_STATE_FAILED);
    }

    /* Sound: one call.  audiodev_init() registers and probes every built-in
     * card (ac97, hda) into the subsys registry with self-tests, and picks
     * the default playback device.  Adding the next codec touches
     * src/sound/audiodev.c's table, not this file. */
    audiodev_init();

    /* The IP stack sits on top of whatever the NIC probe found, so it is
     * configured here and not in e1000_init(): with no card it still comes up
     * with a working loopback, which is all `ping 127.0.0.1` needs. */
    net_init();
    netlink_init(); /* AF_NETLINK: uevent delivery to udev */

    /* ---- filesystem and drivers --------------------------------------- */
    /* Resolve the root the allocator had no hand in earlier: a disk install
     * arrives with no RAM module, and its root is partition 1 of the first
     * ATA disk.  The driver names the disk /dev/sda while probing; the read
     * below bypasses the node entirely and just copies the partition into a
     * fresh 48 MiB arena, which is the only way a mounted filesystem can
     * stay put under a kernel that keeps the whole image resident. */
    if (!root_img) {
        fbcon_puts("no initrd module: reading root from disk 0 partition 1\n");
        ata_init();

        uint64_t frames = 16896; /* 66 MiB: the image is 64 MB plus
                                  * margin for a grown one later */
        uint64_t phys = pmm_alloc_contiguous(frames);
        if (!phys)
            panic("disk root: no memory for the root filesystem");
        root_img  = (uint8_t *)pmm_virt(phys);
        root_size = (uint32_t)ata_read_boot_partition(root_img, (uint32_t)(frames * 4096));
        if (!root_size)
            panic("disk root: no root partition found on disk 0");
        g_bi.initrd_addr = phys;
        g_bi.initrd_size = root_size;
        dbg_puts("GNOS: disk root = ");
        dbg_puts_dec(root_size);
        dbg_puts(" bytes\r\n");
    }

    fbcon_puts("initrd: mounting EXT2 filesystem...\n");
    if (!vfs_init(root_img, root_size))
        panic("initrd is not a mountable ext2 volume");

    /* The tty, sound and pty drivers join the character-device registry.
     * Their /dev nodes appear when chrdev_populate() runs below. */
    pty_init();
    audiodev_vfs_register(); /* /dev/dsp + /dev/snd pair, one call */
    tty_init();

    /* The static character registry (drivers/char): mem + kmsg announce
     * themselves here.  Every other static device (tty, sound, input, fb,
     * DRM) joins the same registry below; chrdev_populate() publishes the
     * whole set once the last driver has registered. */
    chrdev_init();

    /* The evdev driver registers /dev/input/event0 (keyboard) and event1
     * (mouse) over the same i8042 ports tty_init just enabled.  The nodes
     * are published with the rest by chrdev_populate(). */
    input_init();

    /* USB 3.0 (xHCI) comes up after the VFS and the input core, because
     * enumeration now runs through the USB core: HID probes feed the evdev
     * queues immediately and mass-storage probes publish /dev nodes.  The
     * HID class driver needs no further bring-up -- its reports are reaped
     * by usb_hid_poll() from the timer tick. */
    int slot_xhci = subsys_register("xhci", NULL, SUBSYS_CLASS_OTHER, 0, 0);
    if (xhci_init()) {
        subsys_set_state(slot_xhci, SUBSYS_STATE_FAILED);
    } else {
        subsys_set_state(slot_xhci, SUBSYS_STATE_LIVE);
    }

    /* /dev/fb0 joins the character-device registry but shares the framebuffer
     * with fbcon: the boot log stays on screen until a user-space program
     * opens the device and draws over it. */
    fbdev_init(&g_bi);

    /* The DRM/KMS driver registers /dev/dri/card0 and /dev/dri/renderD128 on
     * top of the same framebuffer: dumb-buffer clients modeset through the
     * bochs VBE registers and blit their buffers to the screen.  The ported
     * Uinxed core registers the class, then the dummy driver probes the
     * software KMS pipeline. */
    drm_init();
    drm_init_fallback();

    /* Every static character device has now announced itself; publish the
     * registry in one place.  Runtime devices (pty slaves) publish
     * themselves through cdev_publish_path(). */
    chrdev_populate();

    /* Disks come after the VFS for the same reason /dev/fb0 does -- they are
     * published as /dev nodes and there is no /dev before the mount.  Note
     * the ordering with respect to the *root* filesystem is the other way
     * round from a normal kernel: GNOS boots off an initrd that is already in
     * RAM, so ATA is not on the path to a running system.  It is here for the
     * installer to write to, and a machine with no disk at all boots exactly
     * as far as one with four.  ata_init() registers its own subsys slot,
     * because it discovers how many entries it needs while probing. */
    ata_init();

    /* USB mass storage no longer needs its own bring-up call: the USB core
     * probes BOT interfaces during xHCI enumeration (above) and publishes
     * /dev/sdb through the same block-device layer ATA uses. */

    /* NVMe controllers (class 01/08 on the PCI bus) publish /dev/nvme*N
     * through the same block-device layer.  Like ATA, it is not on the
     * path to a running system -- the root is the initrd image -- but it
     * gives the installer NVMe disks and partitions to work with. */
    nvme_init();

    /* ----. The rest of the root fs setup ---------------------------------
     * At this point the root filesystem is mounted and the disks are up, the
     * two halves a booted-from-disk system is built from.  The live-CD half
     * of the setup (RAM initrd) is out of date only in one respect: it wants
     * the *user space* half of the install machinery to run, and that half
     * needs to keep reading the boot payload from somewhere stable.  What it
     * reads (the kernel, the Limine stage files, the root image itself) was
     * published by this boot under /proc/boot/, which is as far from the
     * disk as it is possible to be: the blobs are boxes that hold the very
     * files an installer dumps onto a drive. */
    procfs_add_blob("/proc/boot/root.img", root_img, root_size);

    syscall_init();

    /* ---- processes ----------------------------------------------------- */
    proc_init();
    timer_init(SCHED_HZ);

    /* The cgroup v2 hierarchy is rooted here, and the canonical mount point
     * is provided up front the way Linux does -- any user space that wants
     * a controller merely writes its control files instead of mounting. */
    cgroup_init();
    vfs_mount_cgroupfs("/sys/fs/cgroup");

    crypto_selftest();
    slab_init();
    pagecache_init();
    pagecache_selftest();
    slab_selftest();
    kheap_self_test();
    gfx_self_test();
    fbdev_self_test();
    acpi_self_test();
    /* Before any process exists, so the two scratch address spaces it borrows
     * cannot collide with a real one: fork() sharing has to be proven before
     * anything relies on it. */
    vmm_cow_self_test();

    /* Last, and only meaningful with a device: it is what decides whether
     * the SVGA II accelerated copy may be used at all, and settles how GMR
     * page descriptors are encoded while it is at it.  Must run after the
     * allocators (it borrows pages) and before user space. */
    svga_selftest();

    subsys_dump();

    /* PID 1 must be the real init: busybox init refuses to start if it
     * is not, which is the whole point of the busybox userland.  Allocate
     * it first so proc_alloc() hands it pid 1, then kthreadd lands on
     * pid 2.  kthreadd will not run until sched_start(); any work queued
     * between now and then is processed once the scheduler is live. */
    fbcon_puts("starting /sbin/init as pid 1...\n\n");

    /* The boot command line (an initrd-root /cmdline file, written by make
     * from KCMD=...) is spliced into PID 1's argv inside proc_spawn_init();
     * g_boot_cmdline is kept here for /proc/cmdline. */
    int pid = proc_spawn_init("/sbin/init");
    if (pid < 0)
        panic("cannot start /sbin/init");

    int kd = proc_spawn_kthreadd();
    if (kd < 0)
        panic("cannot start kthreadd");

    /* init owns the terminal until it hands it to a child of its own. */
    tty_set_pgrp(pid);

    /* Never returns: this thread becomes the idle loop. */
    sched_start();
}

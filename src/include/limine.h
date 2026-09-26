/* SPDX-License-Identifier: GPL-2.0 */
/*
 * limine.h — the Limine boot request/response layout as GNOS uses it. (GPLv2)
 *
 * GNOS is handed control by Limine, and Limine hands it things — a memory
 * map, a framebuffer, the initrd — through a table-driven protocol: the
 * kernel emits a list of 128-bit request IDs inside a marked section, and
 * before the entry point runs Limine walks that list and writes a pointer
 * to a filled-in response body back into each request slot.
 *
 * What is fixed here is the wire format: request IDs, field order and field
 * widths are dictated by the bootloader and must match bit for bit or the
 * handshake silently produces garbage.  Everything else — the comments, the
 * grouping, the names of the plumbing macros — is this file's own writing,
 * so the kernel stays free of the bootloader's licensing terms.
 *
 * Only the requests GNOS actually issues are described below; adding one is
 * a matter of adding its ID and its two structs next to the others.
 */

#ifndef INCLUDE_LIMINE_H_
#define INCLUDE_LIMINE_H_

#include <stdint.h>

/* ---------------------------------------------------------------- request list
 *
 * The linker gathers three sections, in this order, into one contiguous run
 * of objects that Limine scans: a start marker, everything tagged
 * `.limine_requests`, and an end marker.  The values below are magic words
 * chosen by the protocol; they are what makes the section findable in a raw
 * image with no symbol table to go by.
 */

#define LIMINE_REQUESTS_START_MARKER \
    uint64_t limine_requests_start_marker[4] = {0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf, 0x785c6ed015d3e316, 0x181e920a7852b9d9};

#define LIMINE_REQUESTS_END_MARKER \
    uint64_t limine_requests_end_marker[2] = {0xadc0e0531bb10d03, 0x9572709f31764c62};

/* Declares the single base-revision word the section must start with.  The
 * second slot doubles as a reply: a bootloader that understood the request
 * overwrites it with the revision it speaks, so "still holds its initial
 * value" is how the kernel detects an old bootloader.  At the moment GNOS
 * asks for revision 0, the one every Limine release understands. */
#define LIMINE_BASE_REVISION(N) \
    uint64_t limine_base_revision[3] = {0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, (N)};

#define LIMINE_BASE_REVISION_SUPPORTED (limine_base_revision[2] == 0)

/* Every request ID below carries this same 64-bit pair in its first two
 * slots; the second pair is what distinguishes one request from another. */
#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b

/* --------------------------------------------------------------- shared types
 *
 * A file handed over by Limine: anything loaded from the boot medium, so the
 * initrd module and the kernel image itself arrive as one of these. */
struct limine_uuid {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t  d[8];
};

struct limine_file {
    uint64_t            revision;
    void               *address;
    uint64_t            size;
    char               *path;
    char               *cmdline;
    uint32_t            media_type;
    uint32_t            unused;
    uint32_t            tftp_ip;
    uint32_t            tftp_port;
    uint32_t            partition_index;
    uint32_t            mbr_disk_id;
    struct limine_uuid  gpt_disk_uuid;
    struct limine_uuid  gpt_part_uuid;
    struct limine_uuid  part_uuid;
};

/* -------------------------------------------------------------------- HHDM
 *
 * The higher-half direct map: every physical address is also visible at
 * phys + this offset, already mapped for us before the entry point runs.
 * Everything the bootloader hands us points into that window, which is why
 * the kernel can dereference the responses before it owns page tables. */
#define LIMINE_HHDM_REQUEST { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852, 0x63984e959a98244b }

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t                     id[4];
    uint64_t                     revision;
    struct limine_hhdm_response *response;
};

/* ------------------------------------------------------------- framebuffer
 *
 * The console GNOS draws into.  Limine asks the firmware to set a mode and
 * reports what it got, including the exact position of each colour channel
 * inside a pixel, which is the only safe way to paint on both VBE and GOP. */
#define LIMINE_FRAMEBUFFER_REQUEST { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75, 0xa3148604f6fab11b }

#define LIMINE_FRAMEBUFFER_RGB 1

struct limine_framebuffer {
    void    *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;          /* bytes per scanline, may exceed width * bpp / 8 */
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  unused[7];
    uint64_t edid_size;
    void    *edid;
    /* Response revision 1 continues with a list of supported modes.  GNOS
     * asks for revision 0 and takes whatever mode Limine chose, so those
     * trailing fields are deliberately left undescribed. */
};

struct limine_framebuffer_response {
    uint64_t                  revision;
    uint64_t                  framebuffer_count;
    struct limine_framebuffer **framebuffers;
};

struct limine_framebuffer_request {
    uint64_t                            id[4];
    uint64_t                            revision;
    struct limine_framebuffer_response *response;
};

/* ----------------------------------------------------------------- modules
 *
 * Files pulled off the boot medium alongside the kernel.  The ISO ships the
 * initrd as the first one, so it is what becomes the root filesystem. */
#define LIMINE_MODULE_REQUEST { LIMINE_COMMON_MAGIC, 0x3e7e279702be32af, 0xca1c4f3bd1280cee }

struct limine_module_response {
    uint64_t            revision;
    uint64_t            module_count;
    struct limine_file **modules;
};

struct limine_module_request {
    uint64_t                      id[4];
    uint64_t                      revision;
    struct limine_module_response *response;
    /* Request revision 1: modules the bootloader itself must provide. */
    uint64_t                      internal_module_count;
    void                        **internal_modules;
};

/* ------------------------------------------------------------- memory map
 *
 * The authoritative view of physical RAM: what is free, what belongs to the
 * firmware, and what is MMIO nobody should touch.  USABLE is the only type
 * the page allocator may hand out. */
#define LIMINE_MEMMAP_REQUEST { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62 }

#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7
#define LIMINE_MEMMAP_ACPI_TABLES            8

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t                    revision;
    uint64_t                    entry_count;
    struct limine_memmap_entry **entries;
};

struct limine_memmap_request {
    uint64_t                     id[4];
    uint64_t                     revision;
    struct limine_memmap_response *response;
};

/* -------------------------------------------------------------------- RSDP
 *
 * Where ACPI starts.  Asking beats scanning: on UEFI the table is nowhere
 * near the legacy BIOS area the old scan looks in. */
#define LIMINE_RSDP_REQUEST { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43, 0x27637845accdcf3c }

struct limine_rsdp_response {
    uint64_t revision;
    void    *address;
};

struct limine_rsdp_request {
    uint64_t                   id[4];
    uint64_t                   revision;
    struct limine_rsdp_response *response;
};

/* -------------------------------------------------- kernel address request
 *
 * Tells us where Limine placed the kernel image in physical memory and at
 * which virtual address it mapped it, which fixes up every link-time address
 * the kernel compares against. */
#define LIMINE_KERNEL_ADDRESS_REQUEST { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63, 0xb2644a48c516a487 }

struct limine_kernel_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct limine_kernel_address_request {
    uint64_t                              id[4];
    uint64_t                              revision;
    struct limine_kernel_address_response *response;
};

/* ------------------------------------------------------------- command line
 *
 * The boot entry's own cmdline string: the kernel keeps the options it knows
 * and passes the rest to init, which is how `single` reaches user space. */
#define LIMINE_EXECUTABLE_CMDLINE_REQUEST { LIMINE_COMMON_MAGIC, 0x4b161536e598651e, 0xb390ad4a2f1f303a }

struct limine_executable_cmdline_response {
    uint64_t revision;
    char    *cmdline;
};

struct limine_executable_cmdline_request {
    uint64_t                                  id[4];
    uint64_t                                  revision;
    struct limine_executable_cmdline_response *response;
};

/* -------------------------------------------------------------- entry point
 *
 * Rather than letting Limine pick an entry vector by itself, we hand it one
 * function: with this in place Limine lands directly in kernel_entry() with
 * paging already set up. */
#define LIMINE_ENTRY_POINT_REQUEST { LIMINE_COMMON_MAGIC, 0x13d86c035a1cd3e1, 0x2b0caa89d8f3026a }

typedef void (*limine_entry_point)(void);

struct limine_entry_point_response {
    uint64_t revision;
};

struct limine_entry_point_request {
    uint64_t                       id[4];
    uint64_t                       revision;
    struct limine_entry_point_response *response;
    limine_entry_point             entry;
};

/* --------------------------------------------------------------------- SMP
 *
 * Symmetric multiprocessing: Limine parks the application processors and
 * hands us one descriptor per CPU, each carrying a slot for the address the
 * AP should jump to.  Writing that slot releases the AP, so filling them in
 * is the whole of bring-up.  This is the base-revision shape of the request
 * (later revisions renamed it "mp"); the ID is unchanged. */
#define LIMINE_SMP_REQUEST { LIMINE_COMMON_MAGIC, 0x95a67b819a1b857e, 0xa0b61b723b6a73e0 }

#define LIMINE_SMP_X2APIC (1 << 0)

struct limine_smp_info;

typedef void (*limine_goto_address)(struct limine_smp_info *);

struct limine_smp_info {
    uint32_t           processor_id;
    uint32_t           lapic_id;
    uint64_t           reserved;
    limine_goto_address goto_address;   /* write it and the AP starts running */
    uint64_t           extra_argument;
};

struct limine_smp_response {
    uint64_t              revision;
    uint32_t              flags;
    uint32_t              bsp_lapic_id;
    uint64_t              cpu_count;
    struct limine_smp_info **cpus;
};

struct limine_smp_request {
    uint64_t                  id[4];
    uint64_t                  revision;
    struct limine_smp_response *response;
    uint64_t                  flags;
};

#endif // INCLUDE_LIMINE_H_

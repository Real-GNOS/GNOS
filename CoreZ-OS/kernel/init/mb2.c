#include "kernel/init/mb2.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
static struct MB2_INFO g_mb2;
static void parse_tag(const struct MB2_TAG *tag) {
    uint32_t size = tag->size;
    switch (tag->type) {
    case MB2_TYPE_CMDLINE:
        if (size > 8)
            g_mb2.cmdline = (const char *)tag + 8;
        break;
    case MB2_TYPE_BOOT_LOADER_NAME:
        if (size > 8)
            g_mb2.boot_loader_name = (const char *)tag + 8;
        break;
    case MB2_TYPE_MODULE:
        g_mb2.module_count++;
        break;
    case MB2_TYPE_BASIC_MEMINFO:
        if (size >= sizeof(struct MB2_TAG_BASIC_MEMINFO)) {
            const struct MB2_TAG_BASIC_MEMINFO *m =
                (const struct MB2_TAG_BASIC_MEMINFO *)tag;
            g_mb2.mem_lower = m->mem_lower;
            g_mb2.mem_upper = m->mem_upper;
        }
        break;
    case MB2_TYPE_BOOTDEV:
        if (size >= sizeof(struct MB2_TAG_BOOTDEV)) {
            const struct MB2_TAG_BOOTDEV *b = (const struct MB2_TAG_BOOTDEV *)tag;
            g_mb2.has_bootdev = 1;
            g_mb2.biosdev = b->biosdev;
            g_mb2.partition = b->partition;
            g_mb2.sub_partition = b->sub_partition;
        }
        break;
    case MB2_TYPE_LOAD_BASE:
        if (size >= sizeof(struct MB2_TAG_LOAD_BASE)) {
            const struct MB2_TAG_LOAD_BASE *l =
                (const struct MB2_TAG_LOAD_BASE *)tag;
            g_mb2.has_load_base = 1;
            g_mb2.load_base_addr = l->load_base_addr;
        }
        break;
    case MB2_TYPE_MMAP: {
        if (size < sizeof(struct MB2_TAG_MMAP))
            break;
        const struct MB2_TAG_MMAP *m = (const struct MB2_TAG_MMAP *)tag;
        uint32_t esz = (m->entry_size >= sizeof(struct MB2_MMAP_ENTRY))
                           ? m->entry_size
                           : (uint32_t)sizeof(struct MB2_MMAP_ENTRY);
        uint32_t n = (size - 16) / esz;
        g_mb2.has_mmap = 1;
        for (uint32_t i = 0; i < n && i < MB2_MAX_MMAP_ENTRIES; i++) {
            const uint8_t *p = (const uint8_t *)tag + 16 + (uint64_t)i * esz;
            g_mb2.mmap[i] = *(const struct MB2_MMAP_ENTRY *)p;
            g_mb2.mmap_count = i + 1;
        }
        break;
    }
    case MB2_TYPE_FRAMEBUFFER:
        if (size >= 38) {
            g_mb2.has_framebuffer = 1;
            g_mb2.framebuffer = *(const struct MB2_TAG_FRAMEBUFFER *)tag;
        }
        break;
    case MB2_TYPE_ACPI_OLD:
        if (size >= 8 + 20) {
            const uint8_t *p = (const uint8_t *)tag + 8;
            for (int i = 0; i < 20; i++)
                g_mb2.rsdp_old[i] = p[i];
            g_mb2.has_rsdp_old = 1;
        }
        break;
    case MB2_TYPE_ACPI_NEW:
        if (size >= 8 + 36) {
            const uint8_t *p = (const uint8_t *)tag + 8;
            for (int i = 0; i < 36; i++)
                g_mb2.rsdp_new[i] = p[i];
            g_mb2.has_rsdp_new = 1;
        }
        break;
    default:
        break;
    }
}

void mb2_init(uint32_t magic, const void *mbi) {
    memset(&g_mb2, 0, sizeof(g_mb2));
    if (magic != MB2_BOOTLOADER_MAGIC || mbi == 0)
        return;
    g_mb2.valid = 1;
    g_mb2.total_size = *(const uint32_t *)mbi;
    const uint8_t *p = (const uint8_t *)mbi + 8;
    const uint8_t *end = (const uint8_t *)mbi + g_mb2.total_size;
    while (p + sizeof(struct MB2_TAG) <= end) {
        const struct MB2_TAG *tag = (const struct MB2_TAG *)p;
        if (tag->type == MB2_TYPE_END)
            break;
        if (tag->size < sizeof(struct MB2_TAG))
            break;
        parse_tag(tag);
        g_mb2.tag_count++;
        p += (tag->size + 7u) & ~7u;
    }
    if (g_mb2.mem_upper == 0 && g_mb2.mmap_count > 0) {
        uint64_t top = 0;
        for (uint32_t k = 0; k < g_mb2.mmap_count; k++) {
            if (g_mb2.mmap[k].type != MB2_MMAP_AVAILABLE)
                continue;
            uint64_t e = g_mb2.mmap[k].addr + g_mb2.mmap[k].len;
            if (e > top)
                top = e;
        }
        if (top > 0x100000ull)
            g_mb2.mem_upper = (uint32_t)((top - 0x100000ull) >> 10);
    }
}

const struct MB2_INFO *mb2_get(void) {
    return &g_mb2;
}

uint64_t mb2_mem_top(void) {
    uint64_t top = 0;
    for (uint32_t i = 0; i < g_mb2.mmap_count; i++) {
        const struct MB2_MMAP_ENTRY *e = &g_mb2.mmap[i];
        if (e->type != MB2_MMAP_AVAILABLE)
            continue;
        uint64_t end = e->addr + e->len;
        if (end > top)
            top = end;
    }
    if (top == 0 && g_mb2.mem_upper)
        top = ((uint64_t)g_mb2.mem_upper + 1024ull) * 1024ull;
    return top;
}

uint32_t mb2_mem_upper_kb(void) {
    return g_mb2.mem_upper;
}

void mb2_dump(void) {
    const char *lname = "?";
    switch (g_mb2.has_framebuffer ? g_mb2.framebuffer.framebuffer_type : 0xFF) {
    case MB2_FB_TYPE_RGB: lname = "rgb"; break;
    case MB2_FB_TYPE_INDEXED: lname = "indexed"; break;
    case MB2_FB_TYPE_EGA: lname = "ega"; break;
    default: break;
    }
    kprintf("[mb2] tags=%u total=%u cmdline=\"%s\" loader=\"%s\"\n",
            g_mb2.tag_count, g_mb2.total_size,
            g_mb2.cmdline ? g_mb2.cmdline : "",
            g_mb2.boot_loader_name ? g_mb2.boot_loader_name : "");
    kprintf("[mb2] mem_lower=%uKB mem_upper=%uKB mmap=%u entries\n",
            g_mb2.mem_lower, g_mb2.mem_upper, g_mb2.mmap_count);
    if (g_mb2.has_bootdev)
        kprintf("[mb2] bootdev bios=%#x part=%#x sub=%#x\n", g_mb2.biosdev,
                g_mb2.partition, g_mb2.sub_partition);
    if (g_mb2.has_framebuffer)
        kprintf("[mb2] fb %ux%u bpp=%u %s pitch=%u addr=%#x\n",
                g_mb2.framebuffer.framebuffer_width,
                g_mb2.framebuffer.framebuffer_height,
                g_mb2.framebuffer.framebuffer_bpp, lname,
                g_mb2.framebuffer.framebuffer_pitch,
                (uint32_t)g_mb2.framebuffer.framebuffer_addr);
    for (uint32_t i = 0; i < g_mb2.mmap_count && i < 8; i++) {
        const struct MB2_MMAP_ENTRY *e = &g_mb2.mmap[i];
        kprintf("[mb2]  mmap[%u] base=%#x%08x len=%#x%08x type=%u\n", i,
                (uint32_t)(e->addr >> 32), (uint32_t)e->addr,
                (uint32_t)(e->len >> 32), (uint32_t)e->len, e->type);
    }
    kprintf("[mb2] rsdp old=%d new=%d module=%d load_base=%d\n",
            g_mb2.has_rsdp_old, g_mb2.has_rsdp_new, g_mb2.module_count,
            g_mb2.has_load_base);
}

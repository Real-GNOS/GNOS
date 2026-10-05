#include "kernel/init/acpi/acpi.h"
#include "kernel/asm_func.h"
#include "kernel/init/mb2.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "drivers/char/console/io.h"

struct ACPI_RSDP *RSDP = NULL;
struct ACPI_XSDT *XSDT = NULL;
struct ACPI_FADT *FADT = NULL;

#define ACPI_TABLES_VADDR 0x60000000u
#define ACPI_SHARED_PD 0x96000u
#define ACPI_PD_INDEX 256
#define ACPI_MAP_PAGE 0x200000u
#define ACPI_MAP_MAX 128

#define APIC_MMIO_IOAPIC_VADDR 0x40000000u
#define APIC_MMIO_LAPIC_VADDR 0x40200000u
#define APIC_MMIO_PD_IOAPIC 0u
#define APIC_MMIO_PD_LAPIC 1u
#define PTE_KERN_RW 0x83u

static struct ACPI_MADT_INFO s_madt;
static int s_madt_ok;

static uint32_t acpi_mapped_phys[ACPI_MAP_MAX];
static uint32_t acpi_map_nr;

static uintptr_t acpi_va_of(uint32_t phys) {
    uint32_t base = phys & ~(ACPI_MAP_PAGE - 1);
    for (uint32_t i = 0; i < acpi_map_nr; i++) {
        if (acpi_mapped_phys[i] == base)
            return ACPI_TABLES_VADDR + i * ACPI_MAP_PAGE + (phys - base);
    }
    return 0;
}

static void *acpi_map(uint32_t phys, uint32_t size) {
    uint32_t base = phys & ~(ACPI_MAP_PAGE - 1);
    uint32_t last = (phys + size - 1) & ~(ACPI_MAP_PAGE - 1);
    for (;;) {
        if (acpi_va_of(base) == 0) {
            if (acpi_map_nr >= ACPI_MAP_MAX)
                return NULL;

            uint64_t *pd = phys_to_virt(ACPI_SHARED_PD);
            pd[ACPI_PD_INDEX + acpi_map_nr] = (uint64_t)base | 0x83;
            __asm__ volatile(
                "invlpg (%0)"
                :
                : "r"(ACPI_TABLES_VADDR + acpi_map_nr * ACPI_MAP_PAGE)
                : "memory");
            acpi_mapped_phys[acpi_map_nr] = base;
            acpi_map_nr++;
        }
        if (base == last)
            break;
        base += ACPI_MAP_PAGE;
    }
    return (void *)acpi_va_of(phys);
}

static void *acpi_map_table(uint64_t phys) {
    struct ACPI_SDT_HEADER *hdr = (struct ACPI_SDT_HEADER *)acpi_map(
        (uint32_t)phys, sizeof(struct ACPI_SDT_HEADER));
    if (!hdr)
        return NULL;
    return acpi_map((uint32_t)phys, hdr->len);
}

static uint8_t acpi_checksum(uint8_t *addr, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += addr[i];
    return (sum == 0);
}

static struct ACPI_RSDP *acpi_mb2_rsdp(void) {
    const struct MB2_INFO *mb2 = mb2_get();

    if (mb2 == NULL || !mb2->valid)
        return NULL;
    if (mb2->has_rsdp_new) {
        struct ACPI_RSDP *r = (struct ACPI_RSDP *)(uintptr_t)mb2->rsdp_new;
        uint32_t len = (r->revision == 0) ? 20u : r->len;
        if (len >= 20u && len <= (uint32_t)sizeof(mb2->rsdp_new) &&
            memcmp(r->signature, "RSD PTR ", 8) == 0 &&
            acpi_checksum((uint8_t *)r, len))
            return r;
    }
    if (mb2->has_rsdp_old) {
        struct ACPI_RSDP *r = (struct ACPI_RSDP *)(uintptr_t)mb2->rsdp_old;
        if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
            acpi_checksum((uint8_t *)r, 20))
            return r;
    }
    return NULL;
}

static struct ACPI_RSDP *acpi_find_rsdp(void) {
    struct ACPI_RSDP *mb2_rsdp = acpi_mb2_rsdp();
    void *ebda;

    if (mb2_rsdp != NULL)
        return mb2_rsdp;

    ebda = acpi_map(0x000E0000, 0x20000);
    if (!ebda)
        return NULL;

    for (uintptr_t off = 0; off < 0x20000; off += 16) {
        struct ACPI_RSDP *rsdp = (struct ACPI_RSDP *)((uint8_t *)ebda + off);
        if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0) {
            uint32_t len = (rsdp->revision == 0) ? 20 : rsdp->len;
            if (acpi_checksum((unsigned char *)rsdp, len)) {
                return rsdp;
            }
        }
    }
    return NULL;
}

static struct ACPI_SDT_HEADER *acpi_find_table(const char *signature) {
    if (!XSDT) {
        if (RSDP && RSDP->revision >= 2 && RSDP->xsdt_address) {
            XSDT = (struct ACPI_XSDT *)acpi_map_table(RSDP->xsdt_address);
        } else if (RSDP && RSDP->rsdt_address) {
            struct ACPI_RSDT *rsdt =
                (struct ACPI_RSDT *)acpi_map_table(RSDP->rsdt_address);
            if (!rsdt)
                return NULL;
            uint32_t num_entries =
                (rsdt->header.len - sizeof(rsdt->header)) / 4;
            for (uint32_t i = 0; i < num_entries; i++) {
                struct ACPI_SDT_HEADER *hdr =
                    (struct ACPI_SDT_HEADER *)acpi_map_table(rsdt->entry[i]);
                if (hdr && memcmp(hdr->signature, signature, 4) == 0)
                    return hdr;
            }
            return NULL;
        } else {
            return NULL;
        }
    }
    if (!XSDT)
        return NULL;
    if (!acpi_checksum((unsigned char *)XSDT, XSDT->header.len))
        return NULL;

    uint32_t num_entries = (XSDT->header.len - sizeof(XSDT->header)) / 8;
    for (uint32_t i = 0; i < num_entries; i++) {
        struct ACPI_SDT_HEADER *hdr =
            (struct ACPI_SDT_HEADER *)acpi_map_table(XSDT->entry[i]);
        if (hdr && memcmp(hdr->signature, signature, 4) == 0)
            return hdr;
    }
    return NULL;
}

static void madt_remap_mmio(void) {
    uint64_t *pd = phys_to_virt(ACPI_SHARED_PD);

    pd[APIC_MMIO_PD_IOAPIC] =
        (uint64_t)(s_madt.ioapic_addr & ~0xFFFu) | PTE_KERN_RW;
    pd[APIC_MMIO_PD_LAPIC] =
        (uint64_t)(s_madt.lapic_addr & ~0xFFFu) | PTE_KERN_RW;
    __asm__ volatile("invlpg (%0)" ::"r"(APIC_MMIO_IOAPIC_VADDR) : "memory");
    __asm__ volatile("invlpg (%0)" ::"r"(APIC_MMIO_LAPIC_VADDR) : "memory");
}

static void madt_parse(void) {
    struct ACPI_SDT_HEADER *hdr = acpi_find_table("APIC");
    if (!hdr) {
        kprintf("[ACPI] MADT not found, APIC topology unavailable\n");
        return;
    }
    if (hdr->len < ACPI_MADT_FIXED_LEN ||
        !acpi_checksum((uint8_t *)hdr, hdr->len)) {
        kprintf("[ACPI] MADT invalid\n");
        return;
    }

    struct ACPI_MADT *madt = (struct ACPI_MADT *)hdr;
    s_madt.lapic_addr =
        madt->lapic_addr ? madt->lapic_addr : ACPI_APIC_DEFAULT_LAPIC;
    s_madt.ioapic_addr = 0;
    s_madt.ioapic_gsi_base = 0;
    s_madt.lapic_nr = 0;
    for (uint32_t i = 0; i < ACPI_LEGACY_IRQ_NR; i++) {
        s_madt.irq_gsi[i] = i;
        s_madt.irq_flags[i] = 0;
    }

    uint32_t raw_gsi[ACPI_LEGACY_IRQ_NR];
    uint16_t raw_flags[ACPI_LEGACY_IRQ_NR];
    uint8_t raw_seen[ACPI_LEGACY_IRQ_NR];
    memset(raw_seen, 0, sizeof(raw_seen));

    uint8_t *p = (uint8_t *)hdr + ACPI_MADT_FIXED_LEN;
    uint8_t *end = (uint8_t *)hdr + hdr->len;
    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len = p[1];
        if (len < 2 || p + len > end) {
            kprintf("[ACPI] MADT entry truncated at offset %u\n",
                    (unsigned)(p - (uint8_t *)hdr));
            break;
        }
        if (type == ACPI_MADT_TYPE_LAPIC && len >= ACPI_MADT_LAPIC_LEN) {
            struct ACPI_MADT_LAPIC *e = (struct ACPI_MADT_LAPIC *)p;
            if ((e->flags & 1u) && s_madt.lapic_nr < ACPI_MAX_CPUS) {
                s_madt.lapic_apic_id[s_madt.lapic_nr++] = e->apic_id;
            }
        } else if (type == ACPI_MADT_TYPE_IOAPIC && len >= ACPI_MADT_IOAPIC_LEN) {
            struct ACPI_MADT_IOAPIC *e = (struct ACPI_MADT_IOAPIC *)p;
            if (s_madt.ioapic_addr == 0) {
                s_madt.ioapic_addr = e->ioapic_addr;
                s_madt.ioapic_gsi_base = e->gsi_base;
            }
        } else if (type == ACPI_MADT_TYPE_ISO && len >= ACPI_MADT_ISO_LEN) {
            struct ACPI_MADT_ISO *e = (struct ACPI_MADT_ISO *)p;
            if (e->bus == 0 && e->source_irq < ACPI_LEGACY_IRQ_NR) {
                raw_gsi[e->source_irq] = e->gsi;
                raw_flags[e->source_irq] = e->flags;
                raw_seen[e->source_irq] = 1;
            }
        }
        p += len;
    }

    if (s_madt.ioapic_addr == 0) {
        s_madt.ioapic_addr = ACPI_APIC_DEFAULT_IOAPIC;
    }
    for (uint32_t i = 0; i < ACPI_LEGACY_IRQ_NR; i++) {
        if (!raw_seen[i]) {
            continue;
        }
        if (raw_gsi[i] < s_madt.ioapic_gsi_base) {
            kprintf("[ACPI] ISO irq%u -> gsi%u before ioapic base, ignored\n",
                    i, (unsigned)raw_gsi[i]);
            continue;
        }
        s_madt.irq_gsi[i] = raw_gsi[i];
        s_madt.irq_flags[i] = raw_flags[i];
    }

    s_madt_ok = 1;
    kprintf("[ACPI] MADT lapic=0x%x ioapic=0x%x gsi_base=%u cpus=%u\n",
            s_madt.lapic_addr, s_madt.ioapic_addr,
            (unsigned)s_madt.ioapic_gsi_base, (unsigned)s_madt.lapic_nr);
    for (uint32_t i = 0; i < ACPI_LEGACY_IRQ_NR; i++) {
        if (raw_seen[i]) {
            kprintf("[ACPI]   ISO irq%u -> gsi%u flags=0x%x\n", i,
                    (unsigned)s_madt.irq_gsi[i], (unsigned)s_madt.irq_flags[i]);
        }
    }
    madt_remap_mmio();
}

const struct ACPI_MADT_INFO *acpi_madt(void) {
    return s_madt_ok ? &s_madt : NULL;
}

void acpi_init(void) {
    kprintf("[ACPI] searching for RSDP...\n");
    RSDP = acpi_find_rsdp();
    if (!RSDP) {
        kprintf("[ACPI] RSDP not found\n");
        return;
    }
    kprintf("[ACPI] RSDP found, revision %u\n", (unsigned)RSDP->revision);

    if (RSDP->revision >= 2 && RSDP->xsdt_address) {
        kprintf("[ACPI] mapping XSDT at 0x%lx\n", RSDP->xsdt_address);
        XSDT = (struct ACPI_XSDT *)acpi_map_table(RSDP->xsdt_address);
        if (!XSDT || !acpi_checksum((uint8_t *)XSDT, XSDT->header.len)) {
            kprintf("[ACPI] XSDT invalid\n");
            return;
        }
        kprintf("[ACPI] XSDT found, %lu entries\n",
                (XSDT->header.len - sizeof(struct ACPI_SDT_HEADER)) / 8);
    } else if (RSDP->rsdt_address) {
        kprintf("[ACPI] mapping RSDT at 0x%x\n", RSDP->rsdt_address);
        struct ACPI_RSDT *rsdt =
            (struct ACPI_RSDT *)acpi_map_table(RSDP->rsdt_address);
        if (!rsdt || !acpi_checksum((uint8_t *)rsdt, rsdt->header.len)) {
            kprintf("[ACPI] RSDT invalid\n");
            return;
        }
        kprintf("[ACPI] RSDT found\n");
    }

    madt_parse();

    FADT = (struct ACPI_FADT *)acpi_find_table("FACP");
    if (!FADT || !acpi_checksum((uint8_t *)FADT, FADT->header.len)) {
        kprintf("[ACPI] FADT not found or invalid\n");
        return;
    }
    kprintf("[ACPI] FADT found, pm1a=0x%x, smi_cmd=0x%x, acpi_enable=0x%x\n",
            FADT->pm1a_control_block, FADT->smi_command_port,
            FADT->acpi_enable);

    if (!(inw(FADT->pm1a_control_block) & 1)) {
        kprintf("[ACPI] ACPI not enabled, enabling...\n");
        if (FADT->smi_command_port && FADT->acpi_enable) {
            outb(FADT->smi_command_port, FADT->acpi_enable);

            for (int i = 0; i < 300; i++) {
                if (inw(FADT->pm1a_control_block) & 1) {
                    kprintf("[ACPI] ACPI enabled after %d tries\n", i);
                    break;
                }
                for (int j = 0; j < 1000000; j++)
                    asm volatile("pause");
            }
        } else {
            kprintf("[ACPI] smi_cmd=0x%x acpi_enable=0x%x, cannot enable\n",
                    FADT->smi_command_port, FADT->acpi_enable);
        }
    } else {
        kprintf("[ACPI] ACPI already enabled\n");
    }
}

static int acpi_get_s5_slp_typ(uint8_t **slp_typa) {
    struct ACPI_SDT_HEADER *dsdt = acpi_find_table("DSDT");
    if (!dsdt) {
        kprintf("[ACPI] DSDT not found for S5 lookup\n");
        return -1;
    }

    if (!acpi_checksum((uint8_t *)dsdt, dsdt->len)) {
        kprintf("[ACPI] DSDT checksum invalid\n");
        return -1;
    }

    uint8_t *aml = (uint8_t *)dsdt + sizeof(struct ACPI_SDT_HEADER);
    uint32_t aml_len = dsdt->len - sizeof(struct ACPI_SDT_HEADER);
    kprintf("[ACPI] DSDT AML length=%u, scanning for S5 package...\n", aml_len);

    for (uint32_t i = 0; i < aml_len - 3; i++) {
        if (aml[i] == 0x12 && aml[i + 1] == 0x02) {
            uint8_t slp_typ_val = aml[i + 2];
            uint8_t typ_val = aml[i + 3];

            if ((slp_typ_val <= 5) && (typ_val <= 1)) {
                kprintf("[ACPI] Found potential S5 package at offset %u: "
                        "slp_typ=%u, typ=%u\n",
                        i, slp_typ_val, typ_val);
                *slp_typa = &aml[i + 2];
                return 0;
            }
        }
    }

    for (uint32_t i = 0; i < aml_len - 4; i++) {
        if (memcmp(aml + i, "_S5_", 4) == 0) {
            kprintf("[ACPI] Found raw _S5_ at offset %u\n", i);
            uint8_t *p = aml + i + 4;
            for (uint32_t j = 0; j < 128 && p < aml + aml_len - 2; j++, p++) {
                if (*p == 0x12 && *(p + 1) == 0x02) {
                    uint8_t slp_typ_val = *(p + 2);
                    uint8_t typ_val = *(p + 3);
                    kprintf("[ACPI] Parsed S5 package: slp_typ=%u, typ=%u\n",
                            slp_typ_val, typ_val);
                    *slp_typa = p + 2;
                    return 0;
                }
            }
        }
    }

    kprintf("[ACPI] _S5_ not found in DSDT\n");
    return -1;
}

void acpi_shutdown(void) {
    kprintf("[ACPI] shutdown: FADT=%p\n", FADT);
    if (!FADT) {
        kprintf("[ACPI] shutdown aborted: FADT is NULL\n");
        return;
    }

    uint8_t *slp_typa;
    if (acpi_get_s5_slp_typ(&slp_typa) != 0) {
        uint16_t pm1a_cmd = (0 << 10) | (1 << 13);
        outw(FADT->pm1a_control_block, pm1a_cmd);
    } else {
        uint16_t pm1a_cmd = (*slp_typa << 10) | (1 << 13);
        outw(FADT->pm1a_control_block, pm1a_cmd);
    }

    while (1) {
        asm volatile("hlt");
    }
}

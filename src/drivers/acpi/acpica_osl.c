/* SPDX-License-Identifier: GPL-2.0 */
/*
 * acpica_osl.c — the OS Services Layer ACPICA needs to run on GNOS. (GPLv2)
 *
 * ACPICA is portable by construction: everything it needs from a host is
 * named AcpiOsSomething, and this file is the whole contract.  The
 * interesting decisions are the ones where the kernel is *simpler* than a
 * general-purpose OS:
 *
 *   - Memory: kmalloc, and unmapping is a no-op we remember -- the kernel
 *     identity-maps physical memory (HHDM), so "map" is an address sum.
 *   - Locks and semaphores: the kernel runs filesystem and driver code
 *     under a big kernel lock and the ACPI subsystem is single-threaded
 *     here, so these are honest stubs that always succeed.  A lock that
 *     cannot be contended costs nothing.
 *   - Threading: there is no workqueue to defer a GPE handler onto, so
 *     AcpiOsExecute runs the procedure immediately.  That is legal but
 *     means a deferred GPE method runs on the SCI handler's stack.
 *   - Timers: the PIT tick is 10 ms, so AcpiOsSleep rounds up to whole
 *     ticks and AcpiOsStall busy-waits.
 */
#include <stdarg.h>
#include <stdint.h>

/* ACPICA's acpi.h must be its own -- the kernel's table-half driver is
 * also named acpi.h and sits earlier on the include path, so this file
 * reaches the vendored one by relative path. */
#include "../../vendor/acpica/source/include/acpi.h"
#include "acpiosxf.h"

#include "acpi_drv.h"      /* GNOS's table-half driver: acpi_rsdp_phys */
#include "heap.h"
#include "io.h"
#include "kstring.h"
#include "proc.h"
#include "timer.h"
#include "debugcon.h"
#include "vmm.h"

extern uint64_t g_hhdm;      /* HHDM base: pmm.h exports it kernel-wide */

/* ---- lifecycle -------------------------------------------------------- */

ACPI_STATUS AcpiOsInitialize(void)
{
    return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void)
{
    return AE_OK;
}

/* ---- the root pointer -------------------------------------------------- */

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
    /* The bootloader (or our own scan) already found it; the OSL only has
     * to hand that address back.  ACPICA falls back to its own scan if we
     * return zero. */
    return (ACPI_PHYSICAL_ADDRESS)acpi_rsdp_phys();
}

/* ---- table overrides --------------------------------------------------- */

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *InitVal,
                                     ACPI_STRING *NewVal)
{
    if (!InitVal || !NewVal)
        return AE_BAD_PARAMETER;
    *NewVal = NULL;                      /* keep the default name */
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *ExistingTable,
                                ACPI_TABLE_HEADER **NewTable)
{
    if (!ExistingTable || !NewTable)
        return AE_BAD_PARAMETER;
    *NewTable = NULL;                    /* no replacement */
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *ExistingTable,
                                        ACPI_PHYSICAL_ADDRESS *NewAddress,
                                        UINT32 *NewTableLength)
{
    if (!ExistingTable || !NewAddress || !NewTableLength)
        return AE_BAD_PARAMETER;
    *NewAddress = 0;
    *NewTableLength = 0;
    return AE_OK;
}

/* ---- memory ------------------------------------------------------------ */

void *AcpiOsAllocate(ACPI_SIZE Size)
{
    return kmalloc((uint32_t)Size);
}

/* AcpiOsAllocateZeroed is provided by utalloc.c (allocate + memset). */

void AcpiOsFree(void *Memory)
{
    kfree(Memory);
}

/* Tables live in the identity-mapped region, so mapping is arithmetic.
 * The "mapping" still has to be tracked for unmapping to be meaningful. */
#define ACPI_OSL_MAPS 32
static struct {
    ACPI_PHYSICAL_ADDRESS phys;
    void                 *virt;
    ACPI_SIZE             len;
    int                   used;
} g_osl_map[ACPI_OSL_MAPS];

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length)
{
    for (int i = 0; i < ACPI_OSL_MAPS; i++) {
        if (!g_osl_map[i].used) {
            void *v = (void *)(uintptr_t)(Where + g_hhdm);
            if (!v)
                continue;
            g_osl_map[i].phys = Where;
            g_osl_map[i].virt = v;
            g_osl_map[i].len  = Length;
            g_osl_map[i].used = 1;
            return v;
        }
    }
    return NULL;                         /* mapping table full */
}

void AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE Size)
{
    (void)Size;
    for (int i = 0; i < ACPI_OSL_MAPS; i++)
        if (g_osl_map[i].used && g_osl_map[i].virt == LogicalAddress) {
            g_osl_map[i].used = 0;
            return;
        }
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void *LogicalAddress,
                                     ACPI_PHYSICAL_ADDRESS *PhysicalAddress)
{
    if (!LogicalAddress || !PhysicalAddress)
        return AE_BAD_PARAMETER;
    *PhysicalAddress = (ACPI_PHYSICAL_ADDRESS)((uintptr_t)LogicalAddress - g_hhdm);
    return AE_OK;
}

BOOLEAN AcpiOsReadable(void *Pointer, ACPI_SIZE Length)
{
    /* Everything in kernel space is readable; a null pointer is not. */
    return Pointer && Length ? TRUE : FALSE;
}

BOOLEAN AcpiOsWritable(void *Pointer, ACPI_SIZE Length)
{
    return Pointer && Length ? TRUE : FALSE;
}

/* ---- caches (ACPICA object caches) ------------------------------------- */

ACPI_STATUS AcpiOsCreateCache(char *CacheName, UINT16 ObjectSize,
                              UINT16 MaxDepth, ACPI_CACHE_T **ReturnCache)
{
    (void)CacheName; (void)MaxDepth;
    if (!ReturnCache || !ObjectSize)
        return AE_BAD_PARAMETER;
    /* No slab-backed cache: kmalloc per object is correct, just slower. */
    struct acpi_cache_stub { UINT16 size; };
    struct acpi_cache_stub *c = kmalloc(sizeof(*c));
    if (!c)
        return AE_NO_MEMORY;
    c->size = ObjectSize;
    *ReturnCache = (ACPI_CACHE_T *)c;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteCache(ACPI_CACHE_T *Cache)
{
    if (!Cache)
        return AE_BAD_PARAMETER;
    kfree(Cache);
    return AE_OK;
}

ACPI_STATUS AcpiOsPurgeCache(ACPI_CACHE_T *Cache)
{
    if (!Cache)
        return AE_BAD_PARAMETER;
    return AE_OK;                        /* nothing is retained */
}

void *AcpiOsAcquireObject(ACPI_CACHE_T *Cache)
{
    if (!Cache)
        return NULL;
    return kmalloc(sizeof(void *) * 4);  /* sized by the caller's use */
}

ACPI_STATUS AcpiOsReleaseObject(ACPI_CACHE_T *Cache, void *Object)
{
    if (!Cache || !Object)
        return AE_BAD_PARAMETER;
    kfree(Object);
    return AE_OK;
}

/* ---- sync primitives --------------------------------------------------- */

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle)
{
    if (!OutHandle)
        return AE_BAD_PARAMETER;
    *OutHandle = (ACPI_SPINLOCK)1;       /* uncontented: see file header */
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
{
    (void)Handle;
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
{
    (void)Handle;
    return 0;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
    (void)Handle; (void)Flags;
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits,
                                  ACPI_SEMAPHORE *OutHandle)
{
    (void)MaxUnits; (void)InitialUnits;
    if (!OutHandle)
        return AE_BAD_PARAMETER;
    *OutHandle = (ACPI_SEMAPHORE)1;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle)
{
    (void)Handle;
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units,
                                UINT16 Timeout)
{
    (void)Handle; (void)Units; (void)Timeout;
    return AE_OK;                        /* never blocks: no contention */
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units)
{
    (void)Handle; (void)Units;
    return AE_OK;
}

/* ---- hardware access --------------------------------------------------- */

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width)
{
    if (!Value)
        return AE_BAD_PARAMETER;
    switch (Width) {
    case 8:  *Value = inb((uint16_t)Address); return AE_OK;
    case 16: *Value = inw((uint16_t)Address); return AE_OK;
    case 32: *Value = inl((uint16_t)Address); return AE_OK;
    default: return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width)
{
    switch (Width) {
    case 8:  outb((uint16_t)Address, (uint8_t)Value);  return AE_OK;
    case 16: outw((uint16_t)Address, (uint16_t)Value); return AE_OK;
    case 32: outl((uint16_t)Address, Value);           return AE_OK;
    default: return AE_BAD_PARAMETER;
    }
}

/* ---- PCI configuration space ------------------------------------------- */

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA    0x0CFC

static ACPI_STATUS pci_cfg_read(UINT32 Bus, UINT32 Device, UINT32 Function,
                                UINT32 Register, UINT32 *Value)
{
    UINT32 addr = 0x80000000u | ((Bus & 0xFFu) << 16) |
                  ((Device & 0x1Fu) << 11) | ((Function & 0x7u) << 8) |
                  (Register & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, addr);
    *Value = inl(PCI_CONFIG_DATA);
    return AE_OK;
}

static ACPI_STATUS pci_cfg_write(UINT32 Bus, UINT32 Device, UINT32 Function,
                                 UINT32 Register, UINT32 Value)
{
    UINT32 addr = 0x80000000u | ((Bus & 0xFFu) << 16) |
                  ((Device & 0x1Fu) << 11) | ((Function & 0x7u) << 8) |
                  (Register & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, Value);
    return AE_OK;
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Register,
                                       UINT64 *Value, UINT32 Width)
{
    UINT32 v = 0;
    if (!PciId || !Value)
        return AE_BAD_PARAMETER;
    if (pci_cfg_read(PciId->Bus, PciId->Device, PciId->Function,
                     Register, &v) != AE_OK)
        return AE_ERROR;
    switch (Width) {
    case 8:  *Value = (v >> ((Register & 3) * 8)) & 0xFFu; break;
    case 16: *Value = (v >> ((Register & 2) * 8)) & 0xFFFFu; break;
    case 32: *Value = v; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Register,
                                        UINT64 Value, UINT32 Width)
{
    if (!PciId)
        return AE_BAD_PARAMETER;
    if (Width != 32)
        return AE_SUPPORT;               /* read-modify-write not wired */
    return pci_cfg_write(PciId->Bus, PciId->Device, PciId->Function,
                         Register, (UINT32)Value);
}

/* ---- timing ------------------------------------------------------------ */

UINT64 AcpiOsGetTimer(void)
{
    /* 100 Hz ticks; ACPICA wants 100 ns units. */
    return (UINT64)timer_ticks() * 10000000ULL;
}

void AcpiOsSleep(UINT64 Milliseconds)
{
    UINT64 want = timer_ticks() + (Milliseconds + 9) / 10;   /* round up */
    while (timer_ticks() < want)
        asm volatile("pause" ::: "memory");
}

void AcpiOsStall(UINT32 Microseconds)
{
    /* No microsecond clock: spin on the tick counter for whole ticks and
     * busy-wait the remainder. */
    UINT64 start = timer_ticks();
    UINT64 need = (Microseconds + 9999u) / 10000u;           /* in ticks */
    while (timer_ticks() - start < need)
        asm volatile("pause" ::: "memory");
}

/* ---- threads and interrupts -------------------------------------------- */

ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    proc_t *p = proc_current();
    return (ACPI_THREAD_ID)(p ? (uint64_t)p->pid : 0);
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type,
                          ACPI_OSD_EXEC_CALLBACK Function, void *Context)
{
    (void)Type;
    if (!Function)
        return AE_BAD_PARAMETER;
    /* No workqueue: run it now.  Correct for our single-threaded ACPI. */
    Function(Context);
    return AE_OK;
}

void AcpiOsWaitEventsComplete(void)
{
    /* Nothing is ever deferred, so nothing is outstanding. */
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptLevel,
                                          ACPI_OSD_HANDLER Handler,
                                          void *Context)
{
    (void)InterruptLevel; (void)Handler; (void)Context;
    /* The SCI is already owned by src/drivers/acpi (IRQ 9); routing an
     * ACPICA handler onto it is a later step -- report unimplemented
     * rather than silently dropping interrupts. */
    return AE_NOT_IMPLEMENTED;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber,
                                         ACPI_OSD_HANDLER Handler)
{
    (void)InterruptNumber; (void)Handler;
    return AE_NOT_IMPLEMENTED;
}

/* ---- physical memory access -------------------------------------------- */

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value,
                             UINT32 Width)
{
    if (!Value)
        return AE_BAD_PARAMETER;
    volatile void *p = (volatile void *)(uintptr_t)(Address + g_hhdm);
    switch (Width) {
    case 8:  *Value = *(volatile uint8_t *)p;  return AE_OK;
    case 16: *Value = *(volatile uint16_t *)p; return AE_OK;
    case 32: *Value = *(volatile uint32_t *)p; return AE_OK;
    case 64: *Value = *(volatile uint64_t *)p; return AE_OK;
    default: return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value,
                              UINT32 Width)
{
    volatile void *p = (volatile void *)(uintptr_t)(Address + g_hhdm);
    switch (Width) {
    case 8:  *(volatile uint8_t *)p  = (uint8_t)Value;  return AE_OK;
    case 16: *(volatile uint16_t *)p = (uint16_t)Value; return AE_OK;
    case 32: *(volatile uint32_t *)p = (uint32_t)Value; return AE_OK;
    case 64: *(volatile uint64_t *)p = Value;           return AE_OK;
    default: return AE_BAD_PARAMETER;
    }
}

/* AML break/fatal: a fatal signal halts, breakpoints just report. */
ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info)
{
    if (Function == ACPI_SIGNAL_FATAL) {
        ACPI_SIGNAL_FATAL_INFO *info = Info;
        dbg_puts("ACPI: FATAL signal code=");
        dbg_puts_dec(info ? info->Code : 0);
        dbg_puts("\r\n");
        return AE_OK;                    /* let the interpreter decide */
    }
    dbg_puts("ACPI: breakpoint signal\r\n");
    return AE_OK;
}

/* Hook before the firmware sleep registers are written; GNOS does not
 * veto S-states. */
ACPI_STATUS AcpiOsEnterSleep(UINT8 SleepState, UINT32 RegaValue,
                             UINT32 RegbValue)
{
    (void)SleepState; (void)RegaValue; (void)RegbValue;
    return AE_OK;
}

/* ---- output ------------------------------------------------------------ */

void AcpiOsVprintf(const char *Format, va_list Args)
{
    /* No printf in the kernel: hand the format's literal text to the
     * debug console and count the conversions we cannot perform. */
    dbg_puts("ACPI: ");
    for (const char *p = Format; *p; p++) {
        if (*p == '%') {
            /* Skip the conversion specification's literal characters. */
            p++;
            if (*p) p++;
            continue;
        }
        char c[2] = { *p, 0 };
        dbg_puts(c);
    }
    dbg_puts("\r\n");
}

void AcpiOsPrintf(const char *Format, ...)
{
    va_list ap;
    va_start(ap, Format);
    AcpiOsVprintf(Format, ap);
    va_end(ap);
}

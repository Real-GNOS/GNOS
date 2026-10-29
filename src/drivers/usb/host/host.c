/* SPDX-License-Identifier: GPL-2.0 */
/*
 * host.c — USB host-controller abstraction layer. (GPLv2)
 *
 * Ported from the Uinxed USB stack (drivers/usb/host/host.c): the global
 * controller registry, USB bus-number allocation and per-bus device
 * address allocation.  The PCI scan and worker orchestration of the
 * original collapse to the xHCI driver alone -- GNOS has no
 * UHCI/OHCI/EHCI controllers -- so the controller bring-up entry points
 * forward to xhci_init()/xhci_poll().
 */
#include <stdint.h>

#include "heap.h"
#include "smp.h"
#include "vfs.h"
#include "xhci.h"

#include "host.h"
#include "usb.h"

#include "drm_vsnprintf.h" /* plogk */

usb_host_t       *usb_host_list;
static spinlock_t usb_host_lock;
static uint16_t   usb_host_next_bus_number = 1;

/* USB device addresses are 1..127 per bus. Bit 0 is reserved for address 0. */
static uint8_t    usb_address_bitmap[256][16];
static spinlock_t usb_address_lock;

/* Allocate the next unique USB bus number. */
int usb_host_allocate_bus_number(void)
{
    spin_lock(&usb_host_lock);
    if (usb_host_next_bus_number > UINT8_MAX) {
        spin_unlock(&usb_host_lock);
        return -E_NOSPC;
    }
    int bus_number = usb_host_next_bus_number++;
    spin_unlock(&usb_host_lock);
    return bus_number;
}

/* Add a host controller to the global list; a duplicate bus is reported, not returned. */
void usb_host_register(usb_host_t *host)
{
    if (!host || !host->bus_number) {
        plogk("usb: Refusing to register a host controller without a bus number.\n");
        return;
    }
    spin_lock(&usb_host_lock);
    for (usb_host_t *entry = usb_host_list; entry; entry = entry->next) {
        if (entry->bus_number == host->bus_number) {
            spin_unlock(&usb_host_lock);
            plogk("usb: %s: bus %u already in use; controller left unregistered.\n", host->name,
                  host->bus_number);
            return;
        }
    }
    host->next    = usb_host_list;
    usb_host_list = host;
    spin_unlock(&usb_host_lock);
    plogk("usb: Registered %s (bus %u, type %d)\n", host->name, host->bus_number, host->type);
}

/* Remove a host controller from the global list. */
int usb_host_unregister(usb_host_t *host)
{
    if (!host)
        return -E_INVAL;
    spin_lock(&usb_host_lock);
    usb_host_t **prev = &usb_host_list;
    while (*prev) {
        if (*prev == host) {
            *prev = host->next;
            spin_unlock(&usb_host_lock);
            return 0;
        }
        prev = &(*prev)->next;
    }
    spin_unlock(&usb_host_lock);
    return -E_NODEV;
}

/* Look up a host controller by its bus number. */
usb_host_t *usb_host_find_by_bus(uint8_t bus_number)
{
    spin_lock(&usb_host_lock);
    for (usb_host_t *host = usb_host_list; host; host = host->next) {
        if (host->bus_number == bus_number) {
            spin_unlock(&usb_host_lock);
            return host;
        }
    }
    spin_unlock(&usb_host_lock);
    return NULL;
}

/* Look up the index-th host controller of a given type. */
usb_host_t *usb_host_find_by_type(usb_host_type_t type, int index)
{
    spin_lock(&usb_host_lock);
    int count = 0;
    for (usb_host_t *host = usb_host_list; host; host = host->next) {
        if (host->type == type) {
            if (count == index) {
                spin_unlock(&usb_host_lock);
                return host;
            }
            count++;
        }
    }
    spin_unlock(&usb_host_lock);
    return NULL;
}

/* Allocate a free device address on a bus (1..127). */
int usb_host_allocate_address(uint8_t bus_number, uint8_t *address)
{
    if (!address)
        return -E_INVAL;
    spin_lock(&usb_address_lock);
    for (uint8_t addr = 1; addr < 128; addr++) {
        uint8_t byte = addr / 8, bit = addr % 8;
        if (!(usb_address_bitmap[bus_number][byte] & (1u << bit))) {
            usb_address_bitmap[bus_number][byte] |= (1u << bit);
            *address = addr;
            spin_unlock(&usb_address_lock);
            return 0;
        }
    }
    spin_unlock(&usb_address_lock);
    return -E_NOSPC;
}

/* Release a device address on a bus. */
void usb_host_release_address(uint8_t bus_number, uint8_t address)
{
    if (!address || address >= 128)
        return;
    spin_lock(&usb_address_lock);
    uint8_t byte = address / 8, bit = address % 8;
    usb_address_bitmap[bus_number][byte] &= ~(1u << bit);
    spin_unlock(&usb_address_lock);
}

/* Probe the PCI bus for supported USB host controllers (xHCI only in GNOS). */
void usb_host_pci_scan(void)
{
    plogk("usb: Scanning PCI for USB host controllers...\n");
    int count = xhci_init();
    plogk("usb: Found %d controller(s)\n", count);
}

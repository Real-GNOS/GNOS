/* SPDX-License-Identifier: GPL-2.0 */
/*
 * host.h — USB host-controller abstraction layer. (GPLv2)
 *
 * Ported from the Uinxed USB stack (include/drivers/usb/host/host.h),
 * trimmed to the controller types GNOS actually drives.
 */
#ifndef GNUCOS_HOST_H
#define GNUCOS_HOST_H

#include <stdint.h>

#include "usb.h"

typedef enum {
    USB_HOST_UHCI = 0,
    USB_HOST_OHCI,
    USB_HOST_EHCI,
    USB_HOST_XHCI,
} usb_host_type_t;

struct usb_host;

typedef struct usb_host_controller_ops {
    void (*host_stop)(struct usb_host *host);
} usb_host_controller_ops_t;

typedef struct usb_host {
    usb_host_type_t            type;
    uint8_t                    bus_number;
    uint8_t                    max_ports;
    usb_host_controller_ops_t *controller_ops;
    const usb_hcd_ops_t       *hcd_ops;
    void                      *hc_private;
    struct usb_host           *next;
    bool                       running;
    char                       name[16];
} usb_host_t;

extern usb_host_t *usb_host_list;

/* Add a host controller to the global list; duplicates are reported, not returned. */
void usb_host_register(usb_host_t *host);

/* Remove a host controller from the global list. */
int usb_host_unregister(usb_host_t *host);

/* Allocate the next unique USB bus number. */
int usb_host_allocate_bus_number(void);

/* Allocate a free device address on a bus (1..127). */
int usb_host_allocate_address(uint8_t bus_number, uint8_t *address);

/* Release a device address on a bus. */
void usb_host_release_address(uint8_t bus_number, uint8_t address);

/* Probe the PCI bus for all supported USB host controllers. */
void usb_host_pci_scan(void);

/* Look up a host controller by its bus number. */
usb_host_t *usb_host_find_by_bus(uint8_t bus_number);

/* Look up the index-th host controller of a given type. */
usb_host_t *usb_host_find_by_type(usb_host_type_t type, int index);

#endif /* GNUCOS_HOST_H */

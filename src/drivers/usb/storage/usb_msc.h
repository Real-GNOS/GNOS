/*
 * usb_msc.h — USB mass-storage (BOT) driver interface. (GPLv2)
 */
#ifndef GNUCOS_USB_MSC_H
#define GNUCOS_USB_MSC_H

/* Probe every addressed USB device for a bulk-only mass-storage interface
 * and publish it as /dev/sdb.  Call after xhci_init() and ata_init(). */
void usb_msc_init(void);

#endif /* GNUCOS_USB_MSC_H */

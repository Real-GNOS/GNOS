/* SPDX-License-Identifier: GPL-2.0 */
/*
 * usb_hid.h — USB HID keyboard/mouse driver interface. (GPLv2)
 *
 * The class driver registers through the USB core: usb_add_device()
 * dispatches to usb_hid_probe() for each HID-class interface.  Reports are
 * reaped by usb_hid_poll(), called from the timer tick.
 */
#ifndef GNUCOS_USB_HID_H
#define GNUCOS_USB_HID_H

/* Reap armed HID interrupt IN endpoints and decode fresh reports; called
 * from the timer tick. */
void usb_hid_poll(void);

#endif /* GNUCOS_USB_HID_H */

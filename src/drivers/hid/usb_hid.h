/*
 * usb_hid.h — USB HID keyboard/mouse driver interface. (GPLv2)
 */
#ifndef GNUCOS_USB_HID_H
#define GNUCOS_USB_HID_H

/* Probe every addressed USB device for a HID keyboard or mouse and arm
 * their interrupt IN endpoints.  Call after xhci_init() and input_init(). */
void usb_hid_init(void);

/* Reap and decode pending HID reports; called from the timer tick. */
void usb_hid_poll(void);

#endif /* GNUCOS_USB_HID_H */

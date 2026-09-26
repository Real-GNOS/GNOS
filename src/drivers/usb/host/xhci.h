/*
 * xhci.h — the xHCI (USB 3.0) host controller driver. (GPLv2)
 *
 * A from-scratch xHCI implementation for GNOS: register definitions, TRB
 * formats and context structures follow the public xHCI specification
 * (Intel eXtensible Host Controller Interface, rev 1.2).  The driver
 * publishes a small kernel API on top of the controller --
 * xhci_control_transfer()/xhci_bulk_transfer()/xhci_interrupt_transfer() --
 * that the HID keyboard/mouse and mass-storage drivers sit on.
 *
 * The design is deliberately poll-driven like the e1000: transfer calls
 * drain the event ring themselves, and the IRQ handler (when a legacy
 * INTx line is present) reaps HID interrupt reports asynchronously.  No
 * MSI/MSI-X: GNOS's interrupt model is PIC INTx, and QEMU's qemu-xhci
 * falls back to INTx when MSI is not enabled.
 */
#ifndef GNUCOS_XHCI_H
#define GNUCOS_XHCI_H

#include <stdint.h>

#define XHCI_MAX_SLOTS      64
#define XHCI_MAX_PORTS      32
#define XHCI_MAX_EP          32
#define XHCI_TRB_RING_SIZE  256
#define XHCI_EVENT_RING_SIZE 256
#define XHCI_CMD_RING_SIZE  256

#define XHCI_MAX_SCRATCHPAD_BUFFERS 32

#define XHCI_PCI_CLASS    0x0C
#define XHCI_PCI_SUBCLASS 0x03

/* ---- capability registers --------------------------------------------- */
#define XHCI_CAPLENGTH   0x00
#define XHCI_HCSPARAMS1  0x04
#define XHCI_HCSPARAMS2  0x08
#define XHCI_HCSPARAMS3  0x0C
#define XHCI_HCCPARAMS1  0x10
#define XHCI_DBOFF       0x14
#define XHCI_RTSOFF      0x18

/* ---- operational registers -------------------------------------------- */
#define XHCI_USBCMD      0x00
#define XHCI_USBSTS      0x04
#define XHCI_PAGESIZE    0x08
#define XHCI_DNCTRL      0x14
#define XHCI_CRCR_LO     0x18
#define XHCI_CRCR_HI     0x1C
#define XHCI_DCBAAP_LO   0x30
#define XHCI_DCBAAP_HI   0x34
#define XHCI_CONFIG      0x38

#define XHCI_CMD_RS      (1u << 0)
#define XHCI_CMD_HCRST   (1u << 1)
#define XHCI_CMD_INTE    (1u << 2)
#define XHCI_CMD_HSEE    (1u << 3)

#define XHCI_STS_HCH     (1u << 0)
#define XHCI_STS_HSE     (1u << 2)
#define XHCI_STS_EINT    (1u << 3)
#define XHCI_STS_PCD     (1u << 4)
#define XHCI_STS_CNR     (1u << 11)

/* ---- PORTSC ------------------------------------------------------------ */
#define XHCI_PORTSC_CCS   (1u << 0)
#define XHCI_PORTSC_PED   (1u << 1)
#define XHCI_PORTSC_OCA   (1u << 3)
#define XHCI_PORTSC_PR    (1u << 4)
#define XHCI_PORTSC_PLS   (0xFu << 5)
#define XHCI_PORTSC_PP    (1u << 9)
#define XHCI_PORTSC_SPEED (0xFu << 10)
#define XHCI_PORTSC_PIC   (0x3u << 14)
#define XHCI_PORTSC_LWS   (1u << 16)
#define XHCI_PORTSC_CSC   (1u << 17)
#define XHCI_PORTSC_PEC   (1u << 18)
#define XHCI_PORTSC_WRC   (1u << 19)
#define XHCI_PORTSC_OCC   (1u << 20)
#define XHCI_PORTSC_PRC   (1u << 21)
#define XHCI_PORTSC_PLC   (1u << 22)
#define XHCI_PORTSC_CEC   (1u << 23)
#define XHCI_PORTSC_CAS   (1u << 24)
#define XHCI_PORTSC_WCE   (1u << 25)
#define XHCI_PORTSC_WDE   (1u << 26)
#define XHCI_PORTSC_WOE   (1u << 27)
#define XHCI_PORTSC_DR    (1u << 30)
#define XHCI_PORTSC_WPR   (1u << 31)

/* PORTSC mixes RO, RWS, RW1S and RW1C bits.  Like Linux's driver, only
 * write back the RO and the RWS ("0 clears / 1 sets") bits so we never
 * accidentally clear a change bit or disable the port. */
#define XHCI_PORTSC_RO    ((1u << 0) | (1u << 3) | (0xFu << 10) | (1u << 30))
#define XHCI_PORTSC_RWS   ((0xFu << 5) | (1u << 9) | (0x3u << 14) | (0x7u << 25))

#define PORTSC_OFFSET 0x400

/* ---- TRB types --------------------------------------------------------- */
#define TRB_NORMAL         1
#define TRB_SETUP_STAGE    2
#define TRB_DATA_STAGE     3
#define TRB_STATUS_STAGE   4
#define TRB_LINK           6
#define TRB_EVENT_DATA     7
#define TRB_NOOP           8
#define TRB_ENABLE_SLOT    9
#define TRB_DISABLE_SLOT  10
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIGURE_EP  12
#define TRB_EVALUATE_CTX  13
#define TRB_RESET_EP      14
#define TRB_STOP_EP       15
#define TRB_SET_TR_DEQUEUE 16
#define TRB_RESET_DEV     17
#define TRB_FORCE_EVENT   18

#define TRB_EV_TRANSFER   32
#define TRB_EV_CMD_COMP   33
#define TRB_EV_PORT_SC    34
#define TRB_EV_HOST_CTRL  37

#define TRB_C   (1u << 0)
#define TRB_TC  (1u << 1)
#define TRB_CH  (1u << 2)
#define TRB_B   (1u << 4)
#define TRB_BSR (1u << 9)

#define TRB_TRT_NONE      0
#define TRB_TRT_OUT       1
#define TRB_TRT_IN        2

#define TRB_TRT_NO_DATA   0
#define TRB_TRT_OUT_DATA  2
#define TRB_TRT_IN_DATA   3

#define TRB_DIR_IN   (1u << 16)
#define TRB_IDT      (1u << 6)
#define TRB_IOC      (1u << 5)
#define TRB_ISP      (1u << 2)

#define TRB_CYCLE_BIT 1

#define XHCI_COMP_SUCCESS       1
#define XHCI_COMP_SHORT_PACKET 13

/* ---- context types ------------------------------------------------------ */
#define EP_CTX_DISABLED     0
#define EP_CTX_CONTROL      4
#define EP_CTX_BULK_IN      6
#define EP_CTX_INTERRUPT_IN 7

#define CTX_SIZE 32

typedef struct {
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
} xhci_trb_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

/* ---- driver state ------------------------------------------------------- */
typedef struct {
    volatile uint32_t *mmio;         /* BAR0 virtual base */
    volatile uint32_t *op;           /* operational registers */
    volatile uint32_t *doorbell;
    volatile uint32_t *runtime;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_scratchpad;
    uint32_t page_size;

    uint64_t dcbaa_phys;
    uint64_t *dcbaa;
    uint64_t *scratchpad_array;
    uint64_t *scratchpad_buffers[XHCI_MAX_SCRATCHPAD_BUFFERS];

    uint64_t cmd_ring_phys;
    uint64_t *cmd_ring;
    uint32_t cmd_ring_idx;
    uint32_t cmd_ring_cycle;

    uint64_t event_ring_phys;
    uint64_t *event_ring;
    uint64_t *event_ring_seg;
    uint32_t event_ring_idx;
    uint32_t event_ring_cycle;

    uint64_t *device_context_base[XHCI_MAX_SLOTS + 1];
    uint64_t *ep_rings[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint64_t ep_rings_phys[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t ep_ring_idx[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t ep_ring_cycle[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t ep_configured[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint8_t *ep_data_buf[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint64_t ep_data_buf_phys[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t ep_data_buf_len[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    volatile uint32_t ep_has_data[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    volatile uint32_t ep_transfer_residue[XHCI_MAX_SLOTS + 1][XHCI_MAX_EP];
    uint32_t slot_enabled[XHCI_MAX_SLOTS + 1];
    uint8_t  irq;
    uint32_t initialized;
} xhci_t;

/* ---- public API ---------------------------------------------------------- */

/* Allocate a zeroed 4 KiB DMA frame suitable for rings/contexts: 64-byte
 * aligned by construction, with the physical address written to *phys. */
uint64_t *xhci_alloc_page(uint64_t *phys);

/* Probe PCI, init the controller, enumerate ports and address devices.
 * Call once after pci_init().  Returns 0 on success. */
int  xhci_init(void);

/* Drain the event ring (from the IRQ handler and from transfer waits). */
void xhci_poll(void);

/* Number of addressed devices / slot of the idx-th device. */
int  xhci_device_count(void);
int  xhci_device_slot(int idx);

/* Standard USB control transfer on EP0.  Returns bytes transferred or <0. */
int  xhci_control_transfer(int slot, uint8_t bmRequestType, uint8_t bRequest,
                           uint16_t wValue, uint16_t wIndex,
                           void *data, uint16_t wLength);

/* Bulk transfer on a non-control endpoint (ep_addr: 0x81 style). */
int  xhci_bulk_transfer(int slot, int ep_addr, void *data, uint32_t len);

/* One-shot interrupt IN read (HID polling helper). */
int  xhci_interrupt_transfer(int slot, int ep_addr, void *data, uint32_t len,
                             uint32_t interval);

/* Set up an interrupt IN endpoint with a persistent buffer and arm it. */
int  xhci_setup_interrupt_in(int slot, int ep_addr, uint32_t max_packet,
                             uint32_t interval, uint8_t *buf, uint64_t buf_phys);

/* Pull the latest report from an armed interrupt IN endpoint.  Copies at
 * most len bytes of the persistent report buffer into buf and returns the
 * number of bytes, or -1 if no fresh report is pending.  The endpoint is
 * re-armed automatically by xhci_poll() after each completion. */
int  xhci_read_interrupt_report(int slot, int ep_addr, void *buf,
                                uint32_t len);

#endif /* GNUCOS_XHCI_H */

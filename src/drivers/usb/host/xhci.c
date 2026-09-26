/*
 * xhci.c — xHCI host controller driver. (GPLv2)
 *
 * Probe -> reset -> rings -> scratchpad -> start -> port scan -> address
 * devices, then hand transfer primitives to the HID / MSC drivers.  The
 * implementation is self-contained: every DMA structure (rings, contexts,
 * buffers) is a 4 KiB PMM frame, which is 64-byte aligned by construction
 * and has a known physical address -- the two things xHCI demands.
 *
 * Register offsets and TRB layouts follow the public xHCI 1.2 spec.
 */
#include <stdint.h>

#include "xhci.h"
#include "pci.h"
#include "kstring.h"
#include "pmm.h"
#include "heap.h"
#include "io.h"
#include "idt.h"
#include "timer.h"
#include "proc.h"
#include "debugcon.h"

#define XHCI_MMIO_MAP_SIZE    0x10000U
#define XHCI_CONTROLLER_TIMEOUT 1000000U   /* io_delay() spins, worst case */
#define XHCI_PORT_TIMEOUT        300000U
#define XHCI_CMD_TIMEOUT         500000U
#define XHCI_XFER_TIMEOUT        500000U

#define XHCI_STATE_UNINITIALIZED 0U
#define XHCI_STATE_READY         1U
#define XHCI_STATE_FAILED        2U

static xhci_t xhci;

/* ---- register access ---------------------------------------------------- */

static uint32_t xhci_read32(uint32_t off)
{
    return xhci.mmio[off / 4];
}

static void xhci_write32(uint32_t off, uint32_t v)
{
    xhci.mmio[off / 4] = v;
}

static uint32_t op_read32(uint32_t off)
{
    return xhci.op[off / 4];
}

static void op_write32(uint32_t off, uint32_t v)
{
    xhci.op[off / 4] = v;
}

static uint32_t portsc_read(uint32_t port)
{
    return xhci.op[(PORTSC_OFFSET + (port - 1) * 16) / 4];
}

static void portsc_write(uint32_t port, uint32_t v)
{
    xhci.op[(PORTSC_OFFSET + (port - 1) * 16) / 4] = v;
}

static uint32_t portsc_neutral(uint32_t portsc)
{
    return portsc & (XHCI_PORTSC_RO | XHCI_PORTSC_RWS);
}

static void ring_doorbell(uint32_t slot, uint32_t target)
{
    if (xhci.doorbell)
        xhci.doorbell[slot] = target;
}

/* ---- allocation ----------------------------------------------------------
 * xHCI structures need 64-byte alignment and a physical address.  A PMM
 * frame is 4 KiB, naturally aligned; pmm_virt() gives the kernel mapping.
 */

uint64_t *xhci_alloc_page(uint64_t *phys)
{
    uint64_t p = pmm_alloc_zeroed();
    if (!p)
        return NULL;
    *phys = p;
    return (uint64_t *)pmm_virt(p);
}

static void xhci_build_trb(xhci_trb_t *trb, uint64_t param, uint32_t status,
                           uint32_t control)
{
    trb->param0 = (uint32_t)(param & 0xFFFFFFFFu);
    trb->param1 = (uint32_t)(param >> 32);
    trb->param2 = status;
    trb->param3 = control;
}

/* ---- event ring ---------------------------------------------------------- */

static int event_ready(const xhci_trb_t *evt)
{
    return (evt->param3 & TRB_C) == xhci.event_ring_cycle;
}

static void event_advance(void)
{
    xhci.event_ring_idx++;
    if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
        xhci.event_ring_idx = 0;
        xhci.event_ring_cycle ^= 1;
    }
    if (xhci.runtime) {
        uint64_t erdp = xhci.event_ring_phys + xhci.event_ring_idx * 16;
        xhci.runtime[0x38 / 4] = (uint32_t)(erdp & 0xFFFFFFFFu) | (1u << 3);
        xhci.runtime[0x3C / 4] = (uint32_t)(erdp >> 32);
    }
}

/* ---- command ring -------------------------------------------------------- */

static void post_cmd(uint32_t trb_type, uint64_t param, uint32_t status)
{
    uint32_t idx = xhci.cmd_ring_idx;
    xhci_trb_t *trb = &((xhci_trb_t *)xhci.cmd_ring)[idx];

    xhci_build_trb(trb, param, 0,
                   status | (trb_type << 10) |
                   (xhci.cmd_ring_cycle ? TRB_C : 0));
    xhci.cmd_ring_idx++;
    if (xhci.cmd_ring_idx >= XHCI_CMD_RING_SIZE) {
        xhci.cmd_ring_idx = 0;
        xhci.cmd_ring_cycle ^= 1;
    }
    ring_doorbell(0, 0);
}

static int wait_cmd_resp(void)
{
    for (unsigned n = 0; n < XHCI_CMD_TIMEOUT; n++) {
        uint32_t idx = xhci.event_ring_idx;
        xhci_trb_t *evt = &((xhci_trb_t *)xhci.event_ring)[idx];
        if (!event_ready(evt)) {
            io_delay();
            continue;
        }
        uint32_t evt_type = (evt->param3 >> 10) & 0x3F;
        if (evt_type == TRB_EV_CMD_COMP) {
            int code = (evt->param2 >> 24) & 0xFF;
            event_advance();
            return code == XHCI_COMP_SUCCESS ? 0 : -code;
        }
        event_advance();
    }
    dbg_puts("XHCI: command response timeout\r\n");
    return -1;
}

static int send_cmd(uint32_t trb_type, uint64_t param, uint32_t status)
{
    post_cmd(trb_type, param, status);
    return wait_cmd_resp();
}

/* ---- controller lifecycle -------------------------------------------------- */

static int reset_controller(void)
{
    uint32_t cmd = op_read32(XHCI_USBCMD);
    cmd &= ~XHCI_CMD_RS;
    op_write32(XHCI_USBCMD, cmd);

    for (unsigned n = 0; n < XHCI_CONTROLLER_TIMEOUT; n++) {
        if (op_read32(XHCI_USBSTS) & XHCI_STS_HCH)
            break;
        io_delay();
    }
    if (!(op_read32(XHCI_USBSTS) & XHCI_STS_HCH))
        return -1;

    op_write32(XHCI_USBCMD, XHCI_CMD_HCRST);
    for (unsigned n = 0; n < XHCI_CONTROLLER_TIMEOUT; n++) {
        if (!(op_read32(XHCI_USBCMD) & XHCI_CMD_HCRST))
            break;
        io_delay();
    }
    if (op_read32(XHCI_USBCMD) & XHCI_CMD_HCRST)
        return -1;

    for (unsigned n = 0; n < XHCI_CONTROLLER_TIMEOUT; n++) {
        if (!(op_read32(XHCI_USBSTS) & XHCI_STS_CNR))
            break;
        io_delay();
    }
    return (op_read32(XHCI_USBSTS) & XHCI_STS_CNR) ? -1 : 0;
}

static int init_rings(void)
{
    xhci.cmd_ring = xhci_alloc_page(&xhci.cmd_ring_phys);
    if (!xhci.cmd_ring)
        return -1;
    memset(xhci.cmd_ring, 0, XHCI_CMD_RING_SIZE * 16);
    xhci.cmd_ring_idx = 0;
    xhci.cmd_ring_cycle = 1;

    uint64_t seg_phys = 0;
    xhci.event_ring_seg = xhci_alloc_page(&seg_phys);
    if (!xhci.event_ring_seg)
        return -1;

    xhci.event_ring = xhci_alloc_page(&xhci.event_ring_phys);
    if (!xhci.event_ring)
        return -1;
    memset(xhci.event_ring, 0, XHCI_EVENT_RING_SIZE * 16);
    xhci.event_ring_idx = 0;
    xhci.event_ring_cycle = 1;

    xhci.event_ring_seg[0] = xhci.event_ring_phys;
    xhci.event_ring_seg[1] = XHCI_EVENT_RING_SIZE;

    op_write32(XHCI_CRCR_LO, (uint32_t)(xhci.cmd_ring_phys & 0xFFFFFFFFu) | TRB_C);
    op_write32(XHCI_CRCR_HI, (uint32_t)(xhci.cmd_ring_phys >> 32));

    if (!xhci.runtime)
        return -1;
    xhci.runtime[0x28 / 4] = 1;                       /* ERSTSZ */
    xhci.runtime[0x30 / 4] = (uint32_t)(seg_phys & 0xFFFFFFFFu);
    xhci.runtime[0x34 / 4] = (uint32_t)(seg_phys >> 32);
    xhci.runtime[0x38 / 4] = (uint32_t)(xhci.event_ring_phys & 0xFFFFFFFFu);
    xhci.runtime[0x3C / 4] = (uint32_t)(xhci.event_ring_phys >> 32);
    return 0;
}

static int init_scratchpad(void)
{
    xhci.dcbaa = xhci_alloc_page(&xhci.dcbaa_phys);
    if (!xhci.dcbaa)
        return -1;
    memset(xhci.dcbaa, 0, XHCI_MAX_SLOTS * 8 + 16);

    if (xhci.max_scratchpad > XHCI_MAX_SCRATCHPAD_BUFFERS)
        return -1;
    if (xhci.max_scratchpad > 0) {
        uint64_t arr_phys = 0;
        xhci.scratchpad_array = xhci_alloc_page(&arr_phys);
        if (!xhci.scratchpad_array)
            return -1;
        memset(xhci.scratchpad_array, 0, xhci.max_scratchpad * 8);
        for (uint32_t i = 0; i < xhci.max_scratchpad; i++) {
            uint64_t sp_phys = 0;
            xhci.scratchpad_buffers[i] = xhci_alloc_page(&sp_phys);
            if (!xhci.scratchpad_buffers[i])
                return -1;
            memset(xhci.scratchpad_buffers[i], 0, 4096);
            xhci.scratchpad_array[i] = sp_phys;
        }
        xhci.dcbaa[0] = arr_phys;
    }
    op_write32(XHCI_DCBAAP_LO, (uint32_t)(xhci.dcbaa_phys & 0xFFFFFFFFu));
    op_write32(XHCI_DCBAAP_HI, (uint32_t)(xhci.dcbaa_phys >> 32));
    return 0;
}

static int start_controller(void)
{
    uint32_t cmd = op_read32(XHCI_USBCMD);
    op_write32(XHCI_USBCMD, cmd | XHCI_CMD_RS);
    for (unsigned n = 0; n < XHCI_CONTROLLER_TIMEOUT; n++) {
        if (!(op_read32(XHCI_USBSTS) & XHCI_STS_HCH))
            return 0;
        io_delay();
    }
    return -1;
}

/* ---- ports & slots --------------------------------------------------------- */

static int reset_port(uint32_t port)
{
    uint32_t portsc = portsc_read(port);
    portsc_write(port, portsc_neutral(portsc) | XHCI_PORTSC_PR);
    for (unsigned n = 0; n < XHCI_PORT_TIMEOUT; n++) {
        if (!(portsc_read(port) & XHCI_PORTSC_PR))
            break;
        io_delay();
    }
    if (portsc_read(port) & XHCI_PORTSC_PR)
        return -1;
    for (unsigned n = 0; n < XHCI_PORT_TIMEOUT; n++) {
        if (portsc_read(port) & XHCI_PORTSC_PED)
            return 0;
        io_delay();
    }
    return -1;
}

static int setup_device_context(uint32_t slot)
{
    uint64_t ctx_phys = 0;
    uint64_t *ctx = xhci_alloc_page(&ctx_phys);
    if (!ctx)
        return -1;
    memset(ctx, 0, CTX_SIZE * 32);
    xhci.device_context_base[slot] = ctx;
    xhci.dcbaa[slot] = ctx_phys;
    return 0;
}

static int enable_slot(uint32_t *slot_out)
{
    int ret = send_cmd(TRB_ENABLE_SLOT, 0, 0);
    if (ret < 0)
        return ret;
    for (uint32_t i = 1; i <= xhci.max_slots; i++) {
        if (!xhci.slot_enabled[i]) {
            xhci.slot_enabled[i] = 1;
            *slot_out = i;
            return setup_device_context(i);
        }
    }
    return -1;
}

/* ---- transfer rings --------------------------------------------------------- */

static uint64_t *alloc_transfer_ring(uint64_t *phys)
{
    uint64_t *ring = xhci_alloc_page(phys);
    if (!ring)
        return NULL;
    memset(ring, 0, XHCI_TRB_RING_SIZE * 16);
    xhci_trb_t *trb = (xhci_trb_t *)ring;
    xhci_build_trb(&trb[XHCI_TRB_RING_SIZE - 1], *phys, 0,
                   (TRB_LINK << 10) | TRB_TC);
    return ring;
}

static int configure_endpoint(uint32_t slot, uint32_t ep_type, uint32_t ep_id,
                              uint32_t max_packet, uint32_t interval,
                              uint64_t ring_phys)
{
    uint64_t in_phys = 0;
    uint64_t *in = xhci_alloc_page(&in_phys);
    if (!in)
        return -1;
    memset(in, 0, CTX_SIZE * 32);

    uint32_t *ctx = (uint32_t *)in;
    ctx[1] = (1u << 0) | (1u << ep_id);
    ctx[8] = ep_id << 27;

    uint32_t ep_off = 16 + (ep_id - 1) * 8;
    ctx[ep_off + 0] = (interval & 0xFF) << 16;
    ctx[ep_off + 1] = (ep_type << 3) | (max_packet << 16) | (3u << 1);
    ctx[ep_off + 2] = (uint32_t)(ring_phys & 0xFFFFFFF0u) | TRB_C;
    ctx[ep_off + 3] = (uint32_t)(ring_phys >> 32);

    int ret = send_cmd(TRB_CONFIGURE_EP, in_phys, slot << 24);
    return ret;
}

static void post_transfer_trb(uint32_t slot, uint32_t ep_id, uint64_t param,
                              uint32_t status, uint32_t control)
{
    xhci_trb_t *ring = (xhci_trb_t *)xhci.ep_rings[slot][ep_id];
    uint32_t idx = xhci.ep_ring_idx[slot][ep_id];
    uint32_t cycle = xhci.ep_ring_cycle[slot][ep_id];

    if (idx == XHCI_TRB_RING_SIZE - 1) {
        xhci_build_trb(&ring[idx], xhci.ep_rings_phys[slot][ep_id], 0,
                       (TRB_LINK << 10) | TRB_TC | (cycle ? TRB_C : 0));
        idx = 0;
        cycle ^= 1;
    }
    xhci_build_trb(&ring[idx], param, status,
                   control | (cycle ? TRB_C : 0));
    idx++;
    xhci.ep_ring_idx[slot][ep_id] = idx;
    xhci.ep_ring_cycle[slot][ep_id] = cycle;
}

static xhci_trb_t *get_or_create_ep_ring(uint32_t slot, uint32_t ep_id,
                                         uint32_t ep_type,
                                         uint32_t max_packet,
                                         uint32_t interval,
                                         uint64_t *out_phys)
{
    if (slot == 0 || slot > xhci.max_slots || ep_id == 0 ||
        ep_id >= XHCI_MAX_EP)
        return NULL;

    if (xhci.ep_rings[slot][ep_id]) {
        if (out_phys)
            *out_phys = xhci.ep_rings_phys[slot][ep_id];
        return (xhci_trb_t *)xhci.ep_rings[slot][ep_id];
    }

    uint64_t ring_phys = 0;
    uint64_t *ring = alloc_transfer_ring(&ring_phys);
    if (!ring)
        return NULL;

    if (ep_id != 1) {
        if (configure_endpoint(slot, ep_type, ep_id, max_packet,
                               interval, ring_phys) < 0)
            return NULL;
    }

    xhci.ep_rings[slot][ep_id] = ring;
    xhci.ep_rings_phys[slot][ep_id] = ring_phys;
    xhci.ep_ring_idx[slot][ep_id] = 0;
    xhci.ep_ring_cycle[slot][ep_id] = 1;
    xhci.ep_configured[slot][ep_id] = 1;
    return (xhci_trb_t *)ring;
}

static int address_device(uint32_t slot, uint32_t port, uint32_t speed)
{
    uint64_t in_phys = 0;
    uint64_t *in = xhci_alloc_page(&in_phys);
    if (!in)
        return -1;
    memset(in, 0, CTX_SIZE * 32);

    uint64_t ep0_phys = 0;
    uint64_t *ep0 = alloc_transfer_ring(&ep0_phys);
    if (!ep0) {
        return -1;
    }
    xhci.ep_rings[slot][1] = ep0;
    xhci.ep_rings_phys[slot][1] = ep0_phys;
    xhci.ep_ring_idx[slot][1] = 0;
    xhci.ep_ring_cycle[slot][1] = 1;
    xhci.ep_configured[slot][1] = 1;

    uint32_t *ctx = (uint32_t *)in;
    ctx[1] = (1u << 0) | (1u << 1);
    ctx[8] = (1u << 27) | (speed << 20);
    ctx[9] = port << 16;

    uint32_t ep0_off = 16;
    ctx[ep0_off + 1] = (EP_CTX_CONTROL << 3) | (8 << 16) | (3u << 1);
    ctx[ep0_off + 2] = (uint32_t)(ep0_phys & 0xFFFFFFF0u) | TRB_C;
    ctx[ep0_off + 3] = (uint32_t)(ep0_phys >> 32);

    return send_cmd(TRB_ADDRESS_DEV, in_phys, slot << 24);
}

/* ---- transfers ------------------------------------------------------------- */

int xhci_control_transfer(int slot, uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex,
                          void *data, uint16_t wLength)
{
    if (xhci.initialized != XHCI_STATE_READY || slot <= 0 ||
        (uint32_t)slot > xhci.max_slots || !xhci.slot_enabled[slot])
        return -1;

    uint64_t ring_phys;
    if (!get_or_create_ep_ring((uint32_t)slot, 1, EP_CTX_CONTROL, 8, 0,
                               &ring_phys))
        return -1;

    uint64_t data_phys = 0;
    void *data_buf = NULL;
    if (wLength > 0) {
        if (!data)
            return -1;
        data_buf = xhci_alloc_page(&data_phys);
        if (!data_buf)
            return -1;
        if (bmRequestType & 0x80)
            memset(data_buf, 0, wLength);
        else
            memcpy(data_buf, data, wLength);
    }

    int dir_in = (bmRequestType & 0x80) ? 1 : 0;
    uint32_t trt = wLength > 0
        ? (dir_in ? TRB_TRT_IN_DATA : TRB_TRT_OUT_DATA)
        : TRB_TRT_NO_DATA;

    uint64_t setup_word = (uint64_t)bmRequestType |
                          ((uint64_t)bRequest << 8) |
                          ((uint64_t)wValue << 16) |
                          ((uint64_t)wIndex << 32) |
                          ((uint64_t)wLength << 48);

    xhci.ep_has_data[slot][1] = 0;

    post_transfer_trb((uint32_t)slot, 1, setup_word, 8,
                      (TRB_SETUP_STAGE << 10) | (trt << 16) | TRB_IDT);
    if (wLength > 0) {
        uint32_t ctrl = TRB_DATA_STAGE << 10;
        if (dir_in)
            ctrl |= TRB_DIR_IN;
        post_transfer_trb((uint32_t)slot, 1, data_phys, wLength, ctrl);
    }
    uint32_t status_ctrl = (TRB_STATUS_STAGE << 10) | TRB_IOC;
    if (wLength == 0 || !dir_in)
        status_ctrl |= TRB_DIR_IN;
    post_transfer_trb((uint32_t)slot, 1, 0, 0, status_ctrl);

    ring_doorbell((uint32_t)slot, 1);

    int ret = -1;
    for (unsigned n = 0; n < XHCI_XFER_TIMEOUT; n++) {
        xhci_poll();
        uint32_t code = xhci.ep_has_data[slot][1];
        if (code != 0) {
            xhci.ep_has_data[slot][1] = 0;
            if (code == XHCI_COMP_SUCCESS || code == XHCI_COMP_SHORT_PACKET) {
                if (data_buf && dir_in)
                    memcpy(data, data_buf, wLength);
                ret = (int)wLength;
            } else {
                ret = -(int)code;
            }
            break;
        }
        io_delay();
    }
    return ret;
}

int xhci_bulk_transfer(int slot, int ep_addr, void *data, uint32_t len)
{
    if (xhci.initialized != XHCI_STATE_READY || !data || !len || slot <= 0 ||
        (uint32_t)slot > xhci.max_slots || !xhci.slot_enabled[slot])
        return -1;

    int ep_num = ep_addr & 0x0F;
    int is_in = (ep_addr & 0x80) ? 1 : 0;
    int ep_id = ep_num * 2 + is_in;
    uint32_t ep_type = is_in ? EP_CTX_BULK_IN : 2;

    uint64_t ring_phys;
    if (!get_or_create_ep_ring((uint32_t)slot, (uint32_t)ep_id, ep_type,
                               512, 0, &ring_phys))
        return -1;

    uint64_t buf_phys = 0;
    uint8_t *buf = (uint8_t *)xhci_alloc_page(&buf_phys);
    if (!buf)
        return -1;
    if (is_in)
        memset(buf, 0, len);
    else
        memcpy(buf, data, len);

    xhci.ep_has_data[slot][ep_id] = 0;
    post_transfer_trb((uint32_t)slot, (uint32_t)ep_id, buf_phys, len,
                      (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
    ring_doorbell((uint32_t)slot, (uint32_t)ep_id);

    int ret = -1;
    for (unsigned n = 0; n < XHCI_XFER_TIMEOUT; n++) {
        xhci_poll();
        uint32_t code = xhci.ep_has_data[slot][ep_id];
        if (code != 0) {
            xhci.ep_has_data[slot][ep_id] = 0;
            if (code == XHCI_COMP_SUCCESS || code == XHCI_COMP_SHORT_PACKET) {
                uint32_t residue = xhci.ep_transfer_residue[slot][ep_id];
                uint32_t actual = residue <= len ? len - residue : 0;
                if (is_in)
                    memcpy(data, buf, actual);
                ret = (int)actual;
            } else {
                ret = -(int)code;
            }
            break;
        }
        io_delay();
    }
    return ret;
}

int xhci_setup_interrupt_in(int slot, int ep_addr, uint32_t max_packet,
                            uint32_t interval, uint8_t *buf, uint64_t buf_phys)
{
    if (slot <= 0 || (uint32_t)slot > xhci.max_slots ||
        !xhci.slot_enabled[slot])
        return -1;

    int ep_num = ep_addr & 0x0F;
    int ep_id = ep_num * 2 + 1;
    uint32_t mps = max_packet < 8 ? 8 : (max_packet > 64 ? 64 : max_packet);

    uint64_t ring_phys;
    if (!get_or_create_ep_ring((uint32_t)slot, (uint32_t)ep_id,
                               EP_CTX_INTERRUPT_IN, mps, interval,
                               &ring_phys))
        return -1;

    xhci.ep_data_buf[slot][ep_id] = buf;
    xhci.ep_data_buf_phys[slot][ep_id] = buf_phys;
    xhci.ep_data_buf_len[slot][ep_id] = mps;
    xhci.ep_has_data[slot][ep_id] = 0;

    post_transfer_trb((uint32_t)slot, (uint32_t)ep_id, buf_phys, mps,
                      (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
    ring_doorbell((uint32_t)slot, (uint32_t)ep_id);
    return 0;
}

int xhci_read_interrupt_report(int slot, int ep_addr, void *buf,
                               uint32_t len)
{
    if (slot <= 0 || (uint32_t)slot > xhci.max_slots)
        return -1;
    int ep_num = ep_addr & 0x0F;
    int ep_id = ep_num * 2 + 1;
    xhci_poll();
    uint32_t code = xhci.ep_has_data[slot][ep_id];
    if (code != XHCI_COMP_SUCCESS && code != XHCI_COMP_SHORT_PACKET) {
        xhci.ep_has_data[slot][ep_id] = 0;
        return -1;
    }
    xhci.ep_has_data[slot][ep_id] = 0;
    uint32_t mps = xhci.ep_data_buf_len[slot][ep_id];
    uint32_t copy = mps < len ? mps : len;
    if (buf && xhci.ep_data_buf[slot][ep_id])
        memcpy(buf, xhci.ep_data_buf[slot][ep_id], copy);
    return (int)copy;
}

/* ---- event ring poll -------------------------------------------------------- */

void xhci_poll(void)
{
    if (xhci.initialized != XHCI_STATE_READY)
        return;

    uint32_t usbsts = op_read32(XHCI_USBSTS);
    if (!(usbsts & XHCI_STS_EINT))
        return;
    op_write32(XHCI_USBSTS, XHCI_STS_EINT);

    for (;;) {
        uint32_t idx = xhci.event_ring_idx;
        xhci_trb_t *evt = &((xhci_trb_t *)xhci.event_ring)[idx];
        if (!event_ready(evt))
            break;

        uint32_t evt_type = (evt->param3 >> 10) & 0x3F;
        if (evt_type == TRB_EV_TRANSFER) {
            uint32_t slot = (evt->param3 >> 24) & 0xFF;
            uint32_t ep = (evt->param3 >> 16) & 0x1F;
            int code = (evt->param2 >> 24) & 0xFF;
            if (slot > 0 && slot <= xhci.max_slots && ep > 0 &&
                ep < XHCI_MAX_EP) {
                xhci.ep_transfer_residue[slot][ep] = evt->param2 & 0x00FFFFFFu;
                xhci.ep_has_data[slot][ep] = (uint32_t)code;
            }
        } else if (evt_type == TRB_EV_PORT_SC) {
            uint32_t port = (evt->param0 >> 24) & 0xFF;
            uint32_t portsc = portsc_read(port);
            if ((portsc & XHCI_PORTSC_CCS) && !(portsc & XHCI_PORTSC_PED))
                reset_port(port);
        }
        event_advance();
    }
}

/* ---- IRQ ---------------------------------------------------------------------- */

static void xhci_irq(regs_t *r)
{
    (void)r;
    xhci_poll();
}

/* ---- init --------------------------------------------------------------------- */

int xhci_init(void)
{
    if (xhci.initialized != XHCI_STATE_UNINITIALIZED)
        return xhci.initialized == XHCI_STATE_READY ? 0 : -1;

    const pci_dev_t *d = pci_find_class(XHCI_PCI_CLASS, XHCI_PCI_SUBCLASS);
    if (!d || d->progif != 0x30) {
        dbg_puts("XHCI: no USB 3.0 controller on the bus\r\n");
        xhci.initialized = XHCI_STATE_FAILED;
        return -1;
    }

    memset(&xhci, 0, sizeof(xhci));
    pci_enable(d);

    uint64_t bar = pci_map_bar(d, 0);
    if (!bar) {
        dbg_puts("XHCI: BAR0 not mappable\r\n");
        xhci.initialized = XHCI_STATE_FAILED;
        return -1;
    }
    xhci.mmio = (volatile uint32_t *)bar;
    xhci.irq  = d->irq_line;

    uint8_t caplength = (uint8_t)(xhci_read32(XHCI_CAPLENGTH) & 0xFF);
    if (caplength < 0x20) {
        dbg_puts("XHCI: bad capability length\r\n");
        xhci.initialized = XHCI_STATE_FAILED;
        return -1;
    }
    xhci.op = (volatile uint32_t *)((uint8_t *)xhci.mmio + caplength);

    uint32_t hcsparams1 = xhci_read32(XHCI_HCSPARAMS1);
    uint32_t hcsparams2 = xhci_read32(XHCI_HCSPARAMS2);
    uint32_t hccparams1 = xhci_read32(XHCI_HCCPARAMS1);

    /* BIOS legacy handover, if the controller is BIOS-owned. */
    uint32_t ext = (hccparams1 >> 16) << 2;
    for (unsigned hops = 0; ext && ext + 8 <= XHCI_MMIO_MAP_SIZE && hops < 64;
         hops++) {
        uint32_t cap = xhci_read32(ext);
        if ((cap & 0xFF) == 1) {         /* USB Legacy Support */
            uint32_t val = xhci_read32(ext);
            if (val & (1u << 24)) {      /* BIOS owns it */
                val |= (1u << 16);       /* request OS ownership */
                xhci_write32(ext, val);
                for (unsigned n = 0; n < XHCI_CONTROLLER_TIMEOUT; n++) {
                    val = xhci_read32(ext);
                    if (!(val & (1u << 24)))
                        break;
                    io_delay();
                }
                val = xhci_read32(ext);
                if (val & (1u << 24)) {  /* timeout: force */
                    val &= ~(1u << 24);
                    val |= (1u << 16);
                    xhci_write32(ext, val);
                }
            }
            uint32_t legctl = xhci_read32(ext + 4);
            legctl &= ~(0x1Fu | (1u << 15));
            legctl |= 0xE0000000u;
            xhci_write32(ext + 4, legctl);
            break;
        }
        uint32_t next = (cap >> 8) & 0xFF;
        if (!next)
            break;
        ext += next << 2;
    }

    xhci.max_slots = hcsparams1 & 0xFFu;
    if (xhci.max_slots > XHCI_MAX_SLOTS)
        xhci.max_slots = XHCI_MAX_SLOTS;
    xhci.max_ports = (hcsparams1 >> 24) & 0xFFu;
    if (xhci.max_ports > XHCI_MAX_PORTS)
        xhci.max_ports = XHCI_MAX_PORTS;
    xhci.max_scratchpad = (((hcsparams2 >> 27) & 0x1Fu) << 5) |
                          ((hcsparams2 >> 21) & 0x1Fu);
    if (!xhci.max_slots || !xhci.max_ports) {
        xhci.initialized = XHCI_STATE_FAILED;
        return -1;
    }

    uint32_t db_off = xhci_read32(XHCI_DBOFF) & ~0x3u;
    uint32_t rts_off = xhci_read32(XHCI_RTSOFF) & ~0x1Fu;
    xhci.doorbell = (volatile uint32_t *)((uint8_t *)xhci.mmio + db_off);
    xhci.runtime  = (volatile uint32_t *)((uint8_t *)xhci.mmio + rts_off);

    if (reset_controller() < 0) {
        dbg_puts("XHCI: controller reset failed\r\n");
        xhci.initialized = XHCI_STATE_FAILED;
        return -1;
    }
    if (!(op_read32(XHCI_PAGESIZE) & 1u)) {
        dbg_puts("XHCI: no 4 KiB page support\r\n");
        xhci.initialized = XHCI_STATE_FAILED;
        return -1;
    }
    xhci.page_size = 4096;
    if (init_rings() < 0 || init_scratchpad() < 0) {
        dbg_puts("XHCI: ring setup failed\r\n");
        xhci.initialized = XHCI_STATE_FAILED;
        return -1;
    }
    op_write32(XHCI_CONFIG, xhci.max_slots);
    if (start_controller() < 0) {
        dbg_puts("XHCI: start failed\r\n");
        xhci.initialized = XHCI_STATE_FAILED;
        return -1;
    }
    xhci.initialized = XHCI_STATE_READY;

    if (xhci.irq < 16)
        irq_install(xhci.irq, xhci_irq);

    dbg_puts("XHCI: controller up, slots ");
    dbg_puts_dec(xhci.max_slots);
    dbg_puts(", ports ");
    dbg_puts_dec(xhci.max_ports);
    dbg_puts(", irq ");
    dbg_puts_dec(xhci.irq);
    dbg_puts("\r\n");

    timer_delay_ms(100);               /* let ports settle */

    for (uint32_t port = 1; port <= xhci.max_ports; port++) {
        uint32_t portsc = portsc_read(port);
        if (!(portsc & XHCI_PORTSC_CCS))
            continue;
        dbg_puts("XHCI: port ");
        dbg_puts_dec(port);
        dbg_puts(" connected, resetting...\r\n");

        if (reset_port(port) < 0) {
            dbg_puts("XHCI: port reset failed\r\n");
            continue;
        }
        uint32_t slot;
        if (enable_slot(&slot) < 0) {
            dbg_puts("XHCI: enable slot failed\r\n");
            continue;
        }
        uint32_t speed = (portsc_read(port) >> 10) & 0xF;
        if (address_device(slot, port, speed) < 0) {
            dbg_puts("XHCI: address device failed\r\n");
            send_cmd(TRB_DISABLE_SLOT, 0, slot << 24);
            xhci.slot_enabled[slot] = 0;
            continue;
        }
        dbg_puts("XHCI: slot ");
        dbg_puts_dec(slot);
        dbg_puts(" addressed\r\n");
        timer_delay_ms(10);
    }
    return 0;
}

int xhci_device_count(void)
{
    int count = 0;
    for (uint32_t i = 1; i <= xhci.max_slots; i++)
        if (xhci.slot_enabled[i])
            count++;
    return count;
}

int xhci_device_slot(int idx)
{
    for (uint32_t i = 1; i <= xhci.max_slots; i++) {
        if (xhci.slot_enabled[i]) {
            if (idx == 0)
                return (int)i;
            idx--;
        }
    }
    return -1;
}

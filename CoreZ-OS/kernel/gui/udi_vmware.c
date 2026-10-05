#include "kernel/gui/udi.h"
#include "drivers/net/pci.h"

static int vmw_probe(void) {
    uint8_t bus = 0, dev = 0;
    return pci_find_device(0x15AD, 0x0405, &bus, &dev) ? 0 : -1;
}

static int vmw_init(uint32_t *w, uint32_t *h, uint32_t bpp) {
    (void)w;
    (void)h;
    (void)bpp;
    return -1;
}

static int vmw_alloc_buffer(uint32_t w, uint32_t h, uint32_t bpp,
                            struct GUI_UDI_BUFFER *out) {
    (void)w;
    (void)h;
    (void)bpp;
    (void)out;
    return -1;
}

static void vmw_free_buffer(uint64_t handle) {
    (void)handle;
}

static int vmw_commit(uint64_t handle, struct GFX_RECT *rects, int n) {
    (void)handle;
    (void)rects;
    (void)n;
    return -1;
}

static void vmw_wait_vblank(void) {
}

struct GUI_UDI_OPS udi_vmware_ops = {
    "vmware-svga", vmw_probe, vmw_init, vmw_alloc_buffer,
    vmw_free_buffer, vmw_commit, vmw_wait_vblank,
};

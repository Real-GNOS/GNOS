# drm/ — the GNOS DRM/KMS core

Everything in this directory is original GNOS code, released under GPLv2.
There is no code here carried over from another project: the files whose
provenance was unclear were rewritten from their contracts, keeping the
behaviour and the data layouts they had, and expressing them in our own
words.

What lives here is a complete KMS device: a driver, the /dev/dri nodes it
publishes, the objects a client enumerates (CRTC, planes, encoder,
connector, framebuffers, properties, modes), the buffers behind them (GEM),
and the atomic interface that lets a client change the whole configuration
in one go.

## Layout

| Area | Files |
|------|-------|
| Device and driver lifecycle | `drm_drv.c`, `drm_init.c`, `drm_init.h`, `drm_devtmpfs.[ch]` |
| Contract header shared by every module | `drm_device.h` |
| User-facing interface (ioctl numbers and argument structs) | `drm.h`, `drm_mode.h`, `drm_fourcc.h`, `drm_color_mgmt.h` |
| Object model | `drm_crtc.c`, `drm_plane.c`, `drm_encoder.c`, `drm_connector.c`, `drm_mode_object.c`, `drm_property.c`, `drm_framebuffer.c`, `drm_modes.c`, `drm_mode_config.c` |
| Atomic commits | `drm_atomic.c`, `drm_atomic_helper.c`, `drm_atomic_uapi.c` |
| Buffers | `drm_gem.c` |
| Frame pacing | `drm_vblank.c` |
| Per-open state | `drm_file.c`, `drm_auth.c` |
| ioctl dispatch | `drm_ioctl.c` |
| Small utilities | `drm_idr.[ch]`, `drm_hashtab.[ch]`, `drm_mm.[ch]`, `drm_rect.[ch]`, `drm_modeset_lock.[ch]`, `drm_print.[ch]`, `intrusive_list.[ch]`, `rbtree.[ch]` |
| Freestanding shims | `drm_port.h`, `drm_vsnprintf.[ch]`, `drm_libc.c` |
| Carried over, not wired up | `drm_dumb.c`, `drm_event.c`, `drm_kms.c`, `drm_vbe.c`, `drm_internal.h` |

The last row is the older GNOS DRM implementation.  It is kept for
reference and is not part of the build; nothing in the core includes it.

## How the pieces fit

* `drm_init.c` builds the software device: a real KMS pipeline whose scanout
  target is the framebuffer the bootloader set up.  A refresh thread copies
  the client's buffer there once per tick, because a renderer paints into
  its own buffer and never commits a second time.
* `drm_drv.c` owns the device and the per-open state, including the order in
  which a closing client's framebuffers and buffer handles are released.
* Every command goes through `drm_ioctl.c`, which validates the ioctl
  encoding, takes a private copy of the argument, and checks permissions
  before any handler runs.
* Client-visible object ids come from `drm_idr.c`; `drm_device.h` defines the
  types every module shares.

## Verification

Changes here are checked two ways: the kernel must still boot (QEMU, headless,
reaching the login prompt with the same KMS pipeline line), and any struct
whose layout is part of a contract is compared field by field against the
previous version before and after the change.

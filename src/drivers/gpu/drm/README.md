# drm/ — the GNOS DRM/KMS core

Everything in this directory is original GNOS code, released under GPLv2.
The tree was reorganised to follow the module layout of Uinxed-Kernel's DRM
core: one directory per responsibility, several implementation files per
directory, and no flat pile of `drm_*.c` at the top.  The public headers
moved to `src/include/drivers/gpu/drm/`, mirroring the kernel's general
header policy.

What lives here is a complete KMS device: a driver, the /dev/dri nodes it
publishes, the objects a client enumerates (CRTC, planes, encoder,
connector, framebuffers, properties, modes), the buffers behind them (GEM,
dumb buffers, PRIME/dma-buf), and the atomic interface that lets a client
change the whole configuration in one go.

## Layout

| Directory | What goes there |
|-----------|-----------------|
| `core/` | Device and driver lifecycle, `/dev/dri` registration, per-open state, ioctl dispatch, vblank, printing, and the GNOS VFS bridge |
| `gem/` | GEM objects and handles, mmap offset allocation, dumb buffers, framebuffers, and the real dma-buf PRIME implementation |
| `kms/` | Mode objects (CRTC/plane/encoder/connector/property), modes, blending, and the atomic commit machinery |
| `lib/` | idr, hashtab, range allocator, modeset locks, rect helpers, formatting, freestanding C shims, intrusive list and rbtree |
| `simpledrm/` | The software KMS driver, its refresh/copy path, and the Bochs VBE backend |
| `svga/` | QEMU stdvga hardware probe |

File-level map:

| Area | Files |
|------|-------|
| Device and driver lifecycle | `core/drm_drv.c`, `core/drm_init.c` |
| GNOS VFS bridge (`/dev/dri`, per-file callbacks, mmap lookup) | `core/drm_devtmpfs.c`, `core/drm_vfs.c` |
| Contract header shared by every module | `src/include/drivers/gpu/drm/drm_device.h` |
| User-facing interface (ioctl numbers and argument structs) | `src/include/drivers/gpu/drm/drm.h`, `drm_mode.h`, `drm_fourcc.h`, `drm_color_mgmt.h` |
| Object model | `kms/drm_crtc.c`, `drm_plane.c`, `drm_encoder.c`, `drm_connector.c`, `drm_mode_object.c`, `drm_property.c`, `kms/drm_modes.c`, `kms/drm_mode_config.c`, `gem/drm_framebuffer.c` |
| Atomic commits | `kms/drm_atomic.c`, `kms/drm_atomic_helper.c`, `kms/drm_atomic_uapi.c` |
| Buffers | `gem/drm_gem.c`, `gem/drm_gem_dumb.c`, `gem/drm_prime.c` |
| Frame pacing | `core/drm_vblank.c` |
| Per-open state | `core/drm_file.c`, `core/drm_auth.c` |
| ioctl dispatch | `core/drm_ioctl.c` |
| Small utilities | `lib/drm_idr.c`, `lib/drm_hashtab.c`, `lib/drm_mm.c`, `lib/drm_rect.c`, `lib/drm_modeset_lock.c`, `lib/intrusive_list.c`, `lib/rbtree.c` |
| Printing | `core/drm_print.c`, `src/include/drivers/gpu/drm/drm_print.h` |
| Freestanding shims | `drm_port.h`, `lib/drm_vsnprintf.c`, `lib/drm_libc.c` |
| Software driver | `simpledrm/simpledrm.c`, `simpledrm/drm_vbe.c` |
| Hardware probe | `svga/drm_svga.c` |

The old flat files `drm_dumb.c`, `drm_event.c`, `drm_kms.c` and
`drm_internal.h` were not part of the build; their functionality now lives
in `gem/drm_gem_dumb.c`, `core/drm_vblank.c`, `kms/`, and the public
headers under `src/include/drivers/gpu/drm/`.

## How the pieces fit

* `simpledrm/simpledrm.c` builds the software device: a real KMS pipeline
  whose scanout target is the framebuffer the bootloader set up.  It builds
  the pipeline first and only then calls `drm_dev_register()` to publish the
  `/dev/dri` nodes, so no user-space client can see a half-initialised
  device.  A refresh thread copies the client's buffer to the scanout target
  once per tick, because a renderer paints into its own buffer and never
  commits a second time.
* `core/drm_init.c` owns the device registry and class initialisation;
  `drm_init_fallback()` is a thin alias for `simpledrm_init()`.
* `core/drm_devtmpfs.c` is the GNOS VFS bridge.  `/dev/dri` is shared by all
  processes, so the bridge keeps one global `drm_file` and routes every
  open/close/ioctl/mmap through it.  `core/drm_vfs.c` holds the per-file VFS
  callbacks and the mmap lookup.
* `core/drm_drv.c` owns the device and the per-open state, including the
  order in which a closing client's framebuffers and buffer handles are
  released.
* Every command goes through `core/drm_ioctl.c`, which validates the ioctl
  encoding, takes a private copy of the argument, and checks permissions
  before any handler runs.
* Client-visible object ids come from `lib/drm_idr.c`; `drm_device.h`
  defines the types every module shares.

## PRIME is a real dma-buf now

The old PRIME code kept a per-device table of fake descriptors.  It is gone.
Exports now hand out real file descriptors backed by anonymous VFS nodes:

* `drm_gem_prime_export()` creates a `struct dma_buf`, binds it to an
  anonymous node with `vfs_anon_open(VFS_ANON, ...)` + `anon_bind()`, and
  returns the descriptor.  The descriptor can be `dup()`ed and passed over
  a Unix socket like any other fd.
* The dma-buf holds a strong reference on the exporting GEM object.  The
  object's `export_dma_buf` field is only a weak back-pointer, cleared when
  the last descriptor is closed.
* Importing on the same device returns the original GEM object.  Importing
  from another device creates a wrapper object that borrows the backing
  store; the wrapper is not run through the driver's free hook.
* The node's `.release` drops the dma-buf reference.  GNOS has no VMA
  get/put hooks, so a mapping is not counted separately: the descriptor
  must stay open while the mapping is in use, which is exactly what the
  dma-buf ABI requires.
* `dma_buf_node_mmap()` reports physical ranges by translating the
  HHDM-mapped backing pages (`g_hhdm`), so `mmap()` on the descriptor works
  without a per-object VMA layer.

`core/drm_drv.c` calls `drm_prime_init()` from `drm_dev_register()`; on
GNOS the function is intentionally empty because dma-buf uses anonymous
nodes and there is no filesystem to register.

## Verification

Changes here are checked two ways: the kernel must still boot (QEMU, headless,
reaching the login prompt with the same KMS pipeline line), and any struct
whose layout is part of a contract is compared field by field against the
previous version before and after the change.  When a full kernel build is
not available, the DRM sources are still compiled with the kernel's
`-ffreestanding -nostdinc` flags and the full include path so that a broken
prototype or a stale header is caught before commit.

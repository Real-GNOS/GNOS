/*
 * drm_print.h - how the DRM core talks to the console. (GPLv2)
 *
 * A struct drm_printer bundles "somewhere to write" with "how to write
 * there", so that a debug dump aimed at the kernel log can be redirected at
 * a sequence file or swallowed entirely without touching the dumping code.
 * Everything else here is shorthand for the ordinary case: say it once, to
 * the log, tagged so grep can find it.
 */

#ifndef INCLUDE_DRM_DRM_PRINT_H_
#define INCLUDE_DRM_DRM_PRINT_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "debugcon.h"
#include "drm_vsnprintf.h" /* vsnprintf/snprintf/plogk for GNOS */

struct drm_device;

/* Debug categories, matching the classic DRM_UT_* set. */
enum drm_debug_category {
    DRM_UT_CORE   = 0x01,
    DRM_UT_DRIVER = 0x02,
    DRM_UT_KMS    = 0x04,
    DRM_UT_MODE   = 0x08,
    DRM_UT_STATE  = 0x10,
    DRM_UT_LEASE  = 0x20,
    DRM_UT_DP     = 0x40,
    DRM_UT_DRMRES = 0x80,
};

/* Where formatted text goes.  `extra` is owned storage the destructor (such
 * as it is) frees: the printer does not manage it by itself. */
struct drm_printer {
    void (*printfn)(void *arg, const char *fmt, va_list args);
    void (*hex)(void *arg, ...); /* reserved, not used yet */
    void *arg;
    void *extra;
};

#define DRM_PRINTK_FMT "drm: "

/* A printer that sends everything to the kernel log, each line prefixed. */
struct drm_printer drm_printk_printer(const char *prefix);

/* Send one message to @p. */
void drm_vprintf(struct drm_printer *p, const char *fmt, va_list args);

/* printf through a printer. */
void drm_printf(struct drm_printer *p, const char *fmt, ...);

/* Log a message tagged with @level and, when present, the owning device. */
void drm_dev_printk(const struct drm_device *dev, const char *level, const char *fmt, ...);

/* Shorthand for the log; the tags are what you grep for afterwards. */
#define DRM_INFO(fmt, ...)           plogk("drm: " fmt, ##__VA_ARGS__)
#define DRM_ERROR(fmt, ...)          plogk("drm: [error] " fmt, ##__VA_ARGS__)
#define DRM_DEBUG(fmt, ...)          plogk("drm: [debug] " fmt, ##__VA_ARGS__)
#define DRM_DEBUG_KMS(fmt, ...)      plogk("drm: [kms] " fmt, ##__VA_ARGS__)
#define DRM_DEBUG_DRIVER(fmt, ...)   plogk("drm: [drv] " fmt, ##__VA_ARGS__)
#define DRM_WARN(fmt, ...)           plogk("drm: [warn] " fmt, ##__VA_ARGS__)
#define DRM_DEV_ERROR(dev, fmt, ...) drm_dev_printk(dev, "error", fmt, ##__VA_ARGS__)
#define DRM_DEV_INFO(dev, fmt, ...)  drm_dev_printk(dev, "info", fmt, ##__VA_ARGS__)
#define DRM_DEV_WARN(dev, fmt, ...)  drm_dev_printk(dev, "warn", fmt, ##__VA_ARGS__)

#endif /* INCLUDE_DRM_DRM_PRINT_H_ */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_print.c - how the DRM core talks to the console. (GPLv2)
 *
 * One page of glue: turn a printfn callback into something the log can use,
 * and let the rest of the core speak through it without knowing where the
 * text ends up.
 */

#include <stdbool.h>
#include <stdint.h>

#include "drm_port.h" /* malloc -> kmalloc */
#include "drm_print.h"

/*
 * The log printer: stick the prefix in front of the formatted text and hand
 * the whole line to plogk.  The buffer is fixed size on purpose -- this runs
 * in the middle of KMS bring-up, where allocating to print something is
 * exactly the wrong kind of clever.
 */
static void drm_printk_emit(void *arg, const char *fmt, va_list args)
{
    const char *prefix = (const char *)arg;
    char        line[512];
    size_t      used = 0;

    if (prefix != NULL) {
        while (prefix[used] != '\0' && used < sizeof(line) - 1) {
            line[used] = prefix[used];
            used++;
        }
    }

    vsnprintf(line + used, sizeof(line) - used, fmt, args);
    line[sizeof(line) - 1] = '\0';

    plogk("%s", line);
}

struct drm_printer drm_printk_printer(const char *prefix)
{
    struct drm_printer p;
    char              *copy = NULL;

    memset(&p, 0, sizeof(p));
    p.printfn = drm_printk_emit;

    /* The prefix has to outlive the caller's string, so keep our own copy
     * and leave it in ->extra for whoever disposes of the printer.  Without
     * one we degrade to printing without a tag rather than failing. */
    if (prefix != NULL) {
        size_t len = strlen(prefix) + 1;

        copy = malloc(len);
        if (copy != NULL) { memcpy(copy, prefix, len); }
    }

    p.arg   = copy;
    p.extra = copy;

    return p;
}

void drm_vprintf(struct drm_printer *p, const char *fmt, va_list args)
{
    if (p != NULL && p->printfn != NULL) { p->printfn(p->arg, fmt, args); }
}

void drm_printf(struct drm_printer *p, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    drm_vprintf(p, fmt, args);
    va_end(args);
}

void drm_dev_printk(const struct drm_device *dev, const char *level, const char *fmt, ...)
{
    char    prefix[256];
    char    message[512];
    va_list args;

    if (dev != NULL) {
        snprintf(prefix, sizeof(prefix), "drm: [%s] %p ", level, (const void *)dev);
    } else {
        snprintf(prefix, sizeof(prefix), "drm: [%s] ", level);
    }

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    plogk("%s%s", prefix, message);
}

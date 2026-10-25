/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kmsg.c — /dev/kmsg character device.  (GPL-2.0)
 * Writes land in the kernel log (plogk); reads report -EAGAIN because the kernel keeps no
 * queued records — printk output reaches user space through the consoles.
 */

#include "chrdev.h"
#include "heap.h"
#include "kstring.h"
#include "vfs.h"      /* E_* */
#include "drm_vsnprintf.h" /* plogk */

#define KMSG_MAJOR 1
#define KMSG_MINOR 11

/* No queued records are kept; reads report -EAGAIN. */
static int64_t kmsg_read(void *ctx, void *private_data, uint64_t flags, void *buffer,
                         size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)buffer;
    (void)offset;
    (void)size;
    return -E_AGAIN;
}

/* Print the written message to the kernel log. */
static int64_t kmsg_write(void *ctx, void *private_data, uint64_t flags, const void *buffer,
                          size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;
    (void)offset;

    if (!buffer && size)
        return -E_INVAL;
    if (!size)
        return 0;
    if (size > 8192)
        return -E_INVAL;

    char *message = kmalloc(size + 1);
    if (!message) {
        plogk("kmsg: out of memory.\n");
        return -E_NOMEM;
    }
    memcpy(message, buffer, size);
    message[size] = '\0';

    /* /dev/kmsg writers may prefix a syslog priority as "<n>"; strip the
     * framing before the text reaches the console. */
    char *text = message;
    if (text[0] == '<') {
        char *end = strchr(text, '>');
        if (end && (size_t)(end - text) <= 4)
            text = end + 1;
    }
    plogk("%s", text);
    kfree(message);
    return (int64_t)size;
}

/* Register the /dev/kmsg character device. */
void kmsgdev_init(void)
{
    static tmpfs_device_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.file_read  = kmsg_read;
    ops.file_write = kmsg_write;
    if (cdev_add("", "kmsg", KMSG_MAJOR, KMSG_MINOR, 1, (uint16_t)file_stream,
                 0600, &ops) != EOK)
        plogk("chrdev: Cannot register /dev/kmsg\n");
}

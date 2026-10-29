/* SPDX-License-Identifier: GPL-2.0 */
/*
 * chrdev.c — character device registry.  (GPL-2.0)
 * Drivers announce devices with cdev_add_vfs() (native GNOS vfs_ops_t) or
 * cdev_add() (Uinxed tmpfs_device_ops_t).  chrdev_populate() walks the
 * registry once at boot and publishes every node under /dev through the
 * matching backend.
 */

#include "chrdev.h"
#include "heap.h"
#include "kstring.h"
#include "vfs.h"
#include "drm_vsnprintf.h" /* plogk */

static cdev_t *chrdev_list;

/* The dev-table name for a registry entry: "name" or "dir/name". */
static void cdev_flat_name(const cdev_t *cdev, char *out, size_t cap)
{
    if (cdev->dir[0])
        snprintf(out, cap, "%s/%s", cdev->dir, cdev->name);
    else
        snprintf(out, cap, "%s", cdev->name);
}

int cdev_add(const char *dir, const char *name, uint32_t major, uint32_t minor, uint32_t count,
             uint16_t node_type, uint16_t mode, const tmpfs_device_ops_t *ops)
{
    return cdev_add_owned(dir, name, major, minor, count, node_type, mode, 0, 0, ops);
}

int cdev_add_vfs(const char *dir, const char *name, uint32_t major, uint32_t minor, uint32_t count,
                 uint16_t mode, uint16_t uid, uint16_t gid, const vfs_ops_t *vops, void *vpriv)
{
    if (!name || !vops) {
        plogk("chrdev: cdev_add_vfs: missing name or ops.\n");
        return -E_INVAL;
    }

    cdev_t *cdev = kmalloc(sizeof(*cdev));
    if (!cdev)
        return -E_NOMEM;
    memset(cdev, 0, sizeof(*cdev));

    if (dir)
        strncpy(cdev->dir, dir, sizeof(cdev->dir) - 1);
    strncpy(cdev->name, name, sizeof(cdev->name) - 1);
    cdev->major      = major;
    cdev->minor_base = minor;
    cdev->count      = count ? count : 1;
    cdev->mode       = mode;
    cdev->uid        = uid;
    cdev->gid        = gid;
    cdev->vops       = vops;
    cdev->vpriv      = vpriv;

    cdev->next  = chrdev_list;
    chrdev_list = cdev;
    return 0;
}

int cdev_add_owned(const char *dir, const char *name, uint32_t major, uint32_t minor,
                   uint32_t count, uint16_t node_type, uint16_t mode, uint16_t uid, uint16_t gid,
                   const tmpfs_device_ops_t *ops)
{
    if (!name || !ops) {
        plogk("chrdev: cdev_add: missing name or ops.\n");
        return -E_INVAL;
    }

    cdev_t *cdev = kmalloc(sizeof(*cdev));
    if (!cdev) {
        plogk("chrdev: cdev_add: out of memory.\n");
        return -E_NOMEM;
    }
    memset(cdev, 0, sizeof(*cdev));

    if (dir)
        strncpy(cdev->dir, dir, sizeof(cdev->dir) - 1);
    strncpy(cdev->name, name, sizeof(cdev->name) - 1);
    cdev->major      = major;
    cdev->minor_base = minor;
    cdev->count      = count ? count : 1;
    cdev->node_type  = node_type;
    cdev->mode       = mode;
    cdev->uid        = uid;
    cdev->gid        = gid;
    cdev->ops        = *ops;

    cdev->next  = chrdev_list;
    chrdev_list = cdev;
    return 0;
}

int cdev_del(const char *path)
{
    char     full[96];
    cdev_t **link;
    int      status = -E_NOENT;

    if (!path)
        return -E_INVAL;
    snprintf(full, sizeof(full), "/dev/%s", path);

    link = &chrdev_list;
    while (*link) {
        char entry[96];
        snprintf(entry, sizeof(entry), "/dev/%s%s%s", (*link)->dir, (*link)->dir[0] ? "/" : "",
                 (*link)->name);
        if (strcmp(entry, full) == 0) {
            cdev_t *victim = *link;
            char    flat[96];
            cdev_flat_name(victim, flat, sizeof(flat));
            *link = victim->next;
            kfree(victim);
            vfs_unregister_dev(flat); /* node goes with the registry entry */
            status = 0;
            break;
        }
        link = &(*link)->next;
    }
    return status;
}

/* Copy the cdev at a registry index, or return 0 at the end of the list. */
static int chrdev_at(int index, cdev_t *out)
{
    if (!out || index < 0)
        return 0;
    cdev_t *cdev = chrdev_list;
    for (int i = 0; cdev && i < index; i++)
        cdev = cdev->next;
    if (!cdev)
        return 0;
    *out      = *cdev;
    out->next = NULL;
    return 1;
}

int chrdev_lookup(uint32_t major, uint32_t minor, cdev_t *out)
{
    if (!out)
        return 0;
    for (cdev_t *cdev = chrdev_list; cdev; cdev = cdev->next) {
        if (cdev->major != major)
            continue;
        if (minor < cdev->minor_base || minor >= cdev->minor_base + cdev->count)
            continue;
        *out      = *cdev;
        out->next = NULL;
        return 1;
    }
    return 0;
}

int chrdev_lookup_path(const char *rel, cdev_t *out)
{
    if (!rel || !out)
        return 0;
    for (cdev_t *cdev = chrdev_list; cdev; cdev = cdev->next) {
        /* "dir/name" vs rel; a registry entry with no dir matches a bare
         * leaf name only. */
        if (cdev->dir[0]) {
            size_t dl = strlen(cdev->dir);
            if (strncmp(rel, cdev->dir, dl) != 0 || rel[dl] != '/')
                continue;
            if (strcmp(rel + dl + 1, cdev->name) != 0)
                continue;
        } else if (strcmp(rel, cdev->name) != 0) {
            continue;
        }
        *out      = *cdev;
        out->next = NULL;
        return 1;
    }
    return 0;
}

/* Create the /dev node for one registry entry, on whichever backend the
 * entry declared: native vfs_ops_t straight into the dev table, or the
 * Uinxed tmpfs ops through the devtmpfs bridge the DRM port brought in. */
static int cdev_publish(const cdev_t *cdev)
{
    char flat[96], path[112];
    cdev_flat_name(cdev, flat, sizeof(flat));

    if (cdev->vops) {
        if (vfs_register_devnum(flat, cdev->vops, cdev->vpriv, cdev->major, cdev->minor_base) != 0)
            return -E_INVAL;
    } else {
        snprintf(path, sizeof(path), "/dev/%s", flat);
        if (devtmpfs_register_char_device(
                path, MKDEV(cdev->major, cdev->minor_base), MKDEV(cdev->major, cdev->minor_base),
                (int)cdev->node_type, (tmpfs_device_ops_t *)&cdev->ops) != 0)
            return -E_INVAL;
    }
    /* Push the registry's ownership/mode into the dev table so the VFS
     * open-path permission check and stat see the real values. */
    if (cdev->mode)
        vfs_dev_set_perm(flat, cdev->mode, cdev->uid, cdev->gid);
    return 0;
}

int cdev_publish_path(const char *dir, const char *name)
{
    /* chrdev_lookup_path takes the /dev-relative path: the leaf itself for
     * root entries, "dir/name" for entries under a subdirectory. */
    char rel[96];
    if (dir[0])
        snprintf(rel, sizeof(rel), "%s/%s", dir, name);
    else
        snprintf(rel, sizeof(rel), "%s", name);

    cdev_t cdev;
    if (!chrdev_lookup_path(rel, &cdev) || strcmp(cdev.name, name) != 0)
        return -E_NOENT;
    return cdev_publish(&cdev);
}

int chrdev_populate(void)
{
    int    count = 0;
    cdev_t cdev;

    for (int index = 0; chrdev_at(index, &cdev); index++) {
        char base[32];
        for (uint32_t i = 0; i < cdev.count; i++) {
            if (cdev.count > 1) {
                if (i == 0)
                    snprintf(base, sizeof(base), "%s", cdev.name);
                snprintf(cdev.name, sizeof(cdev.name), "%s%u", base, cdev.minor_base + i);
            }
            if (cdev_publish(&cdev) == 0)
                count++;
        }
    }
    return count;
}

void chrdev_init(void)
{
    memdev_init();
    kmsgdev_init();
}

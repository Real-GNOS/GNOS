/*
 * drm_auth.c - the old "magic cookie" handshake, and mastership. (GPLv2)
 *
 * Before there were render nodes, a client that wanted to do privileged
 * things proved it by a dance: the display server asked the kernel for a
 * magic number, passed it to the client, and the client handed it back.
 * Recognising a number we issued to *this* file is what makes it safe --
 * the whole value of the exchange is that the number never leaves the two
 * parties.
 *
 * Mastership is the other half: exactly one client at a time owns the
 * display, and SET_MASTER / DROP_MASTER is how it takes and gives it back.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm.h"
#include "drm_device.h"
#include "drm_hashtab.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

/*
 * One master: the client that currently owns the device.  The magic table
 * here is the one a *master* keeps on behalf of the clients it vouches for.
 */
struct drm_master {
    struct drm_device   *dev;
    spinlock_t           lock;
    int                  unique_len;
    char                *unique;
    struct drm_open_hash magiclist;
    ilist_node_t         magicfree;
    int                  refcount;
};

/* Magics are handed out in sequence; 0 is never used, so it stays invalid. */
static drm_magic_t magic_counter = 1;

/* DRM_IOCTL_GET_MAGIC: mint a number for this file and remember it. */
int drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_auth      *auth;
    struct drm_hash_item *item;

    (void)dev;

    if (data == NULL || file_priv == NULL) { return -EINVAL; }

    auth = (struct drm_auth *)data;

    item = malloc(sizeof(*item));
    if (item == NULL) { return -ENOMEM; }
    memset(item, 0, sizeof(*item));

    spin_lock(&file_priv->magic_lock);
    item->key = (unsigned long)magic_counter++;
    spin_unlock(&file_priv->magic_lock);

    if (drm_ht_insert_item(&file_priv->magiclist, item) != 0) {
        free(item);
        return -ENOMEM;
    }

    auth->magic = (drm_magic_t)item->key;
    return 0;
}

/*
 * DRM_IOCTL_AUTH_MAGIC: accept a number issued to this file and mark the
 * file authenticated.  A number we never issued -- or issued to somebody
 * else -- simply is not in the table.
 */
int drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_auth      *auth;
    struct drm_hash_item *item;

    (void)dev;

    if (data == NULL || file_priv == NULL) { return -EINVAL; }

    auth = (struct drm_auth *)data;

    spin_lock(&file_priv->magic_lock);
    if (drm_ht_find_item(&file_priv->magiclist, (unsigned long)auth->magic, &item) != 0) {
        spin_unlock(&file_priv->magic_lock);
        return -EINVAL;
    }
    spin_unlock(&file_priv->magic_lock);

    file_priv->authenticated = true;
    return 0;
}

/* DRM_IOCTL_SET_MASTER: become the owning client. */
int drm_setmaster(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_master *master;

    (void)data;

    if (dev == NULL || file_priv == NULL) { return -EINVAL; }
    if (file_priv->master != NULL) { return -EINVAL; }

    plogk("drm: SET_MASTER called\n");

    master = malloc(sizeof(*master));
    if (master == NULL) { return -ENOMEM; }
    memset(master, 0, sizeof(*master));

    master->dev      = dev;
    master->refcount = 1;

    if (drm_ht_create(&master->magiclist, 4) != 0) {
        free(master);
        return -ENOMEM;
    }

    ilist_init(&master->magicfree);

    file_priv->master = master;
    return 0;
}

/* DRM_IOCTL_DROP_MASTER: hand ownership back. */
int drm_dropmaster(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_master *master;

    (void)dev;
    (void)data;

    if (file_priv == NULL) { return -EINVAL; }

    master = file_priv->master;
    if (master == NULL) { return -EINVAL; }

    drm_ht_destroy(&master->magiclist);
    free(master);
    file_priv->master = NULL;

    return 0;
}

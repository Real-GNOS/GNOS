/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sysfs.c — /sys, the kernel's exported object hierarchy. (GPLv2)
 *
 * Rebuilt on the kobject model ported from the Uinxed driver-model base:
 * the tree IS the kobject hierarchy, and an attribute is a struct attribute
 * owned by a kobject whose ktype's sysfs_ops dispatch show()/store().
 *
 * The VFS sees /sys through the same path-prefix hooks as /proc and /debug,
 * but unlike them the tree is live: sysfs_resolve() walks kobject children
 * per lookup, so directories appear and disappear as the device model
 * registers and unregisters objects -- there is no registration table to
 * keep in sync and no cached VFS state to invalidate.
 *
 * Open nodes carry the matched sysfs entry in node->priv (the attribute
 * entry owns its kobject for dispatch); entries only go away through
 * sysfs_remove_*(), so a node that outlives its entry is the usual
 * use-after-unregister window every GNOS pseudo-filesystem shares.
 *
 * The legacy sysfs_add_file() API survives as a thin adapter for the
 * pre-model call sites (built-in kernel attributes, DRM's card0 status):
 * it find-or-creates the kobject directory chain and hangs a generated
 * attribute off the leaf.
 */
#include <stdint.h>

#include "heap.h"
#include "kobject.h"
#include "kstring.h"
#include "sysfs.h"
#include "timer.h"
#include "vfs.h"

#include "drm_vsnprintf.h" /* vsnprintf/snprintf for sysfs_emit() */

/* ---- internal entry types ------------------------------------------------ */

typedef struct sysfs_attr_entry {
    struct attribute *attr;  /* the attribute (name + mode) */
    struct kobject   *owner; /* kobject whose ktype dispatches show/store */
    uint16_t          mode;
} sysfs_attr_entry_t;

typedef struct sysfs_bin_attr_entry {
    struct bin_attribute *attr;
    struct kobject       *owner;
    uint16_t              mode;
} sysfs_bin_attr_entry_t;

typedef struct sysfs_symlink_entry {
    char           *name;   /* heap copy; the entry owns it */
    struct kobject *target; /* referenced for the entry's lifetime */
} sysfs_symlink_entry_t;

struct kobject *sysfs_root_kobj;      /* /sys root kobject (global) */
struct kobject *sysfs_dev_char_kobj;  /* /sys/dev/char  */
struct kobject *sysfs_dev_block_kobj; /* /sys/dev/block */

/* What a resolved path component ended on. */
typedef enum leaf_kind {
    LEAF_NONE,
    LEAF_DIR, /* a kobject (directory) */
    LEAF_ATTR,
    LEAF_BIN,
    LEAF_SYM,
} leaf_kind_t;

typedef struct sysfs_leaf {
    leaf_kind_t             kind;
    struct kobject         *dir;  /* kobject containing the leaf */
    struct kobject         *kobj; /* LEAF_DIR: the kobject itself */
    sysfs_attr_entry_t     *attr;
    sysfs_bin_attr_entry_t *bin;
    sysfs_symlink_entry_t  *sym;
    char                    name[KOBJ_NAME_LEN]; /* last component */
} sysfs_leaf_t;

/* ---- emit helpers -------------------------------------------------------- */

/* Emit formatted output into a sysfs buffer at offset 0 */
__attribute__((format(printf, 2, 3))) int sysfs_emit(char *buf, const char *fmt, ...)
{
    va_list args;
    int     n;

    if (!buf)
        return 0;

    va_start(args, fmt);
    n = vsnprintf(buf, SYSFS_PAGE_SIZE, fmt, args);
    va_end(args);

    return n;
}

/* Emit formatted output into a sysfs buffer at a given offset */
__attribute__((format(printf, 3, 4))) int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
    va_list args;
    int     n;

    if (!buf)
        return 0;
    if (at < 0 || at >= SYSFS_PAGE_SIZE)
        return 0;

    va_start(args, fmt);
    n = vsnprintf(buf + at, SYSFS_PAGE_SIZE - at, fmt, args);
    va_end(args);

    return n;
}

/* ---- entry lookup -------------------------------------------------------- */

/* Look up an attribute entry by name */
static sysfs_attr_entry_t *sysfs_find_attr(struct kobject *kobj, const char *name)
{
    clist_t node;

    if (!kobj || !name)
        return NULL;
    for (node = kobj->attributes; node; node = node->next) {
        sysfs_attr_entry_t *entry = node->data;
        if (entry && entry->attr && entry->attr->name && streq(entry->attr->name, name))
            return entry;
    }
    return NULL;
}

/* Look up a binary attribute entry by name. */
static sysfs_bin_attr_entry_t *sysfs_find_bin_attr(struct kobject *kobj, const char *name)
{
    if (!kobj || !name)
        return NULL;
    for (clist_t node = kobj->bin_attributes; node; node = node->next) {
        sysfs_bin_attr_entry_t *entry = node->data;
        if (entry && entry->attr && entry->attr->attr.name && streq(entry->attr->attr.name, name))
            return entry;
    }
    return NULL;
}

/* Look up a symlink entry by name */
static sysfs_symlink_entry_t *sysfs_find_symlink(struct kobject *kobj, const char *name)
{
    clist_t node;

    if (!kobj || !name)
        return NULL;
    for (node = kobj->symlinks; node; node = node->next) {
        sysfs_symlink_entry_t *entry = node->data;
        if (entry && entry->name && streq(entry->name, name))
            return entry;
    }
    return NULL;
}

/* Accept non-empty names that contain no path separators. */
static int sysfs_name_valid(const char *name)
{
    return name && name[0] && !strchr(name, '/') && !streq(name, ".") && !streq(name, "..");
}

/* Whether a name collides with an existing child, attribute, or symlink. */
static int sysfs_name_exists(struct kobject *kobj, const char *name)
{
    return kobject_find_child(kobj, name) || sysfs_find_attr(kobj, name) ||
           sysfs_find_bin_attr(kobj, name) || sysfs_find_symlink(kobj, name);
}

/* ---- symlink targets ----------------------------------------------------- */

/* Build a relative path from one kobject to another. */
static char *sysfs_relative_path(struct kobject *from, struct kobject *target)
{
    struct kobject *from_path[64];
    struct kobject *target_path[64];
    int             from_depth   = 0;
    int             target_depth = 0;

    for (struct kobject *node = from; node && from_depth < 64; node = node->parent)
        from_path[from_depth++] = node;
    for (struct kobject *node = target; node && target_depth < 64; node = node->parent)
        target_path[target_depth++] = node;
    if (!from_depth || !target_depth || from_path[from_depth - 1] != target_path[target_depth - 1])
        return NULL;

    int from_index   = from_depth - 1;
    int target_index = target_depth - 1;
    while (from_index >= 0 && target_index >= 0 &&
           from_path[from_index] == target_path[target_index]) {
        from_index--;
        target_index--;
    }

    size_t length = (size_t)(from_index + 1) * 3;
    for (int i = target_index; i >= 0; i--)
        length += strlen(target_path[i]->name) + (i ? 1 : 0);
    if (!length)
        length = 1;

    char *path = kmalloc(length + 1);
    if (!path)
        return NULL;
    path[0] = '\0';
    for (int i = 0; i <= from_index; i++)
        strcat(path, "../");
    for (int i = target_index; i >= 0; i--) {
        strcat(path, target_path[i]->name);
        if (i)
            strcat(path, "/");
    }
    if (!path[0])
        strcpy(path, ".");
    return path;
}

/* ---- legacy sysfs_add_file adapter --------------------------------------- */

/*
 * Pre-model call sites register a generator/handler pair per path.  The
 * adapter turns each into a kobject directory chain plus a heap attribute
 * whose ktype dispatches to the pair.  Legacy attributes are never removed
 * (their callers register once at boot), so the heap payload lives as long
 * as the tree does.
 */
typedef struct legacy_ent {
    sysfs_gen_t      gen;
    sysfs_put_t      put;
    struct attribute attr;
} legacy_ent_t;

static ssize_t legacy_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    legacy_ent_t *e   = container_of(attr, legacy_ent_t, attr);
    uint32_t      len = 0;

    (void)kobj;
    e->gen(buf, SYSFS_PAGE_SIZE, &len);
    if (len > SYSFS_PAGE_SIZE)
        return -E_OVERFLOW;
    return (ssize_t)len;
}

static ssize_t legacy_store(struct kobject *kobj, struct attribute *attr, const char *buf,
                            size_t count)
{
    legacy_ent_t *e = container_of(attr, legacy_ent_t, attr);

    (void)kobj;
    if (!e->put)
        return -E_ACCES;
    int r = e->put(buf, (uint32_t)count);
    return r < 0 ? (ssize_t)r : (ssize_t)count;
}

static void legacy_kobj_release(struct kobject *kobj)
{
    kfree(kobj);
}

static const struct sysfs_ops legacy_sysfs_ops = {
    .show  = legacy_show,
    .store = legacy_store,
};

static struct kobj_type legacy_ktype = {
    .release   = legacy_kobj_release,
    .sysfs_ops = &legacy_sysfs_ops,
};

/* ---- built-in kernel attributes ------------------------------------------ */

static void put_str(char *buf, uint32_t cap, uint32_t *len, const char *s)
{
    while (*s && *len + 1 < cap)
        buf[(*len)++] = *s++;
    if (*len < cap)
        buf[*len] = '\0';
}

static void put_dec(char *buf, uint32_t cap, uint32_t *len, uint64_t v)
{
    char tmp[24];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n && *len + 1 < cap)
        buf[(*len)++] = tmp[--n];
    if (*len < cap)
        buf[*len] = '\0';
}

static void gen_ostype(char *buf, uint32_t cap, uint32_t *len)
{
    put_str(buf, cap, len, "GNOS\n");
}

static void gen_osrelease(char *buf, uint32_t cap, uint32_t *len)
{
    put_str(buf, cap, len, "0.1\n");
}

static void gen_uptime(char *buf, uint32_t cap, uint32_t *len)
{
    put_dec(buf, cap, len, timer_ticks() / SCHED_HZ);
    put_str(buf, cap, len, "\n");
}

/* ---- root kobject bootstrap ---------------------------------------------- */

/* Free the sysfs root kobject. */
static void sysfs_root_release(struct kobject *kobj)
{
    kfree(kobj);
}

static struct kobj_type sysfs_root_ktype = {
    .release = sysfs_root_release,
};

/*
 * Create the root kobject, the standard top-level directories and the
 * built-in kernel attributes, once, the first time anything touches /sys
 * after the heap is up.  sysfs_init() runs before kheap_init() in the boot
 * sequence, so it merely attempts this; every later entry point retries
 * through here.  Idempotent.
 */
static int sysfs_ensure_root(void)
{
    static const char *const top_level_names[] = {
        "block", "bus", "class", "dev", "devices", "firmware", "fs", "kernel", "module", "power",
    };

    if (sysfs_root_kobj)
        return 0;

    struct kobject *root = kcalloc(1, sizeof(struct kobject));
    if (!root)
        return -E_NOMEM;

    kobject_init(root, &sysfs_root_ktype);
    int ret = kobject_set_name(root, "%s", "sys");
    if (ret != 0) {
        kfree(root);
        return ret;
    }
    root->state_in_sysfs = 1;

    /*
     * Top-level directories.  Since the GNOS VFS materialises nodes on
     * demand, "creating a directory" is just adding the child kobject.
     */
    for (size_t i = 0; i < sizeof(top_level_names) / sizeof(top_level_names[0]); i++) {
        if (!kobject_create_and_add(top_level_names[i], root)) {
            ret = -E_NOMEM;
            goto err_children;
        }
    }

    /*
     * /sys/dev/char and /sys/dev/block hold the major:minor -> device
     * symlinks that udev uses to resolve a device number to a node.
     */
    struct kobject *dev_kobj = kobject_find_child(root, "dev");
    if (dev_kobj) {
        sysfs_dev_char_kobj  = kobject_create_and_add("char", dev_kobj);
        sysfs_dev_block_kobj = kobject_create_and_add("block", dev_kobj);
        if (!sysfs_dev_char_kobj || !sysfs_dev_block_kobj) {
            ret = -E_NOMEM;
            goto err_children;
        }
    }

    /* Publish only once the whole tree is up: a partial root would race a
     * concurrent reader into a half-populated /sys. */
    sysfs_root_kobj = root;

    /* Built-in attributes under /sys/kernel.  Cosmetic on failure. */
    if (sysfs_add_file("kernel/ostype", gen_ostype, NULL) != 0 ||
        sysfs_add_file("kernel/osrelease", gen_osrelease, NULL) != 0 ||
        sysfs_add_file("kernel/uptime", gen_uptime, NULL) != 0)
        plogk("sysfs: built-in attribute registration incomplete\n");

    return 0;
err_children:
    while (root->children) {
        struct kobject *child = root->children->data;
        kobject_put(child);
    }
    plogk("sysfs: root kobject init failed (%d)\n", ret);
    kfree(root);
    return ret;
}

/* Create the sysfs root kobject and the standard top-level directories. */
int sysfs_kobject_init(void)
{
    return sysfs_ensure_root();
}

/* ---- path walk ----------------------------------------------------------- */

/*
 * Walk `rel` (a path below /sys, no leading slash) through the kobject
 * tree.  On success fills `leaf` and returns 0; -E_INVAL for a malformed
 * path, -E_NOENT for an unknown leaf, -E_NAMETOOLONG for a component that
 * cannot be a kobject name.  Attribute and symlink entries are only valid
 * as the last component -- sysfs has no files below files.
 */
static int sysfs_walk(const char *rel, sysfs_leaf_t *leaf)
{
    memset(leaf, 0, sizeof(*leaf));
    if (!sysfs_ensure_root()) {
        /* root present */
    } else {
        return -E_NOENT;
    }

    struct kobject *cur = sysfs_root_kobj;
    const char     *p   = rel;

    if (!p || !p[0]) {
        leaf->kind = LEAF_DIR;
        leaf->dir  = cur;
        leaf->kobj = cur;
        strncpy(leaf->name, "sys", KOBJ_NAME_LEN - 1);
        return 0;
    }

    for (;;) {
        const char *slash = strchr(p, '/');
        size_t      clen  = slash ? (size_t)(slash - p) : strlen(p);
        if (!clen)
            return -E_INVAL;
        if (clen >= KOBJ_NAME_LEN)
            return -E_NAMETOOLONG;

        char comp[KOBJ_NAME_LEN];
        memcpy(comp, p, clen);
        comp[clen] = '\0';
        int last   = !slash;

        struct kobject *child = kobject_find_child(cur, comp);
        if (child) {
            if (last) {
                leaf->kind = LEAF_DIR;
                leaf->dir  = cur;
                leaf->kobj = child;
                strncpy(leaf->name, comp, KOBJ_NAME_LEN - 1);
                return 0;
            }
            cur = child;
            p   = slash + 1;
            continue;
        }

        if (!last)
            return -E_NOENT; /* attributes are always leaves */

        sysfs_attr_entry_t *attr = sysfs_find_attr(cur, comp);
        if (attr) {
            leaf->kind = LEAF_ATTR;
            leaf->dir  = cur;
            leaf->attr = attr;
            strncpy(leaf->name, comp, KOBJ_NAME_LEN - 1);
            return 0;
        }
        sysfs_bin_attr_entry_t *bin = sysfs_find_bin_attr(cur, comp);
        if (bin) {
            leaf->kind = LEAF_BIN;
            leaf->dir  = cur;
            leaf->bin  = bin;
            strncpy(leaf->name, comp, KOBJ_NAME_LEN - 1);
            return 0;
        }
        sysfs_symlink_entry_t *sym = sysfs_find_symlink(cur, comp);
        if (sym) {
            leaf->kind = LEAF_SYM;
            leaf->dir  = cur;
            leaf->sym  = sym;
            strncpy(leaf->name, comp, KOBJ_NAME_LEN - 1);
            return 0;
        }
        return -E_NOENT;
    }
}

/* ---- node fill ----------------------------------------------------------- */

static int32_t sysfs_dir_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n;
    (void)off;
    (void)buf;
    (void)len;
    return -E_ISDIR;
}

static int32_t sysfs_dir_write(vfs_node_t *n, uint64_t off, const void *buf, uint32_t len)
{
    (void)n;
    (void)off;
    (void)buf;
    (void)len;
    return -E_INVAL;
}

static const vfs_ops_t g_sysfs_dir_ops = {.read = sysfs_dir_read, .write = sysfs_dir_write};

/* Attribute content read: regenerate through the owner ktype, then slice. */
static int32_t sysfs_attr_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    sysfs_attr_entry_t *e = n->priv;
    if (!e || !e->owner)
        return -E_INVAL;
    struct kobject *kobj = e->owner;
    if (!kobj->ktype || !kobj->ktype->sysfs_ops || !kobj->ktype->sysfs_ops->show)
        return -E_INVAL;

    char *page = kmalloc(SYSFS_PAGE_SIZE);
    if (!page)
        return -E_NOMEM;
    ssize_t r = kobj->ktype->sysfs_ops->show(kobj, e->attr, page);
    if (r < 0) {
        kfree(page);
        return (int32_t)r;
    }
    if (off >= (uint64_t)r) {
        kfree(page);
        return 0;
    }
    uint32_t avail = (uint32_t)((uint64_t)r - off);
    if (len > avail)
        len = avail;
    memcpy(buf, page + off, len);
    kfree(page);
    return (int32_t)len;
}

/* Attribute content write: single-shot store at offset 0. */
static int32_t sysfs_attr_write(vfs_node_t *n, uint64_t off, const void *buf, uint32_t len)
{
    sysfs_attr_entry_t *e = n->priv;
    if (!e || !e->owner)
        return -E_INVAL;
    if (off != 0)
        return -E_INVAL;
    if (len >= SYSFS_PAGE_SIZE)
        return -E_RANGE;
    if (!(e->mode & 0200))
        return -E_ACCES;
    struct kobject *kobj = e->owner;
    if (!kobj->ktype || !kobj->ktype->sysfs_ops || !kobj->ktype->sysfs_ops->store)
        return -E_INVAL;

    char *copy = kmalloc(len + 1);
    if (!copy)
        return -E_NOMEM;
    memcpy(copy, buf, len);
    copy[len] = '\0';
    ssize_t r = kobj->ktype->sysfs_ops->store(kobj, e->attr, copy, len);
    kfree(copy);
    if (r < 0)
        return (int32_t)r;
    return (int32_t)len;
}

static const vfs_ops_t g_sysfs_attr_ops = {.read = sysfs_attr_read, .write = sysfs_attr_write};

/* Binary attribute read/write: straight through to the callbacks. */
static int32_t sysfs_bin_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    sysfs_bin_attr_entry_t *e = n->priv;
    if (!e || !e->owner || !e->attr || !e->attr->read)
        return -E_INVAL;
    return (int32_t)e->attr->read(e->owner, e->attr, buf, (int64_t)off, len);
}

static int32_t sysfs_bin_write(vfs_node_t *n, uint64_t off, const void *buf, uint32_t len)
{
    sysfs_bin_attr_entry_t *e = n->priv;
    if (!e || !e->owner || !e->attr || !e->attr->write)
        return -E_INVAL;
    if (!(e->mode & 0200))
        return -E_ACCES;
    return (int32_t)e->attr->write(e->owner, e->attr, (char *)buf, (int64_t)off, len);
}

static const vfs_ops_t g_sysfs_bin_ops = {.read = sysfs_bin_read, .write = sysfs_bin_write};

/* Symlinks answer readlink(2) through the VFS hook, not read(2). */
static int32_t sysfs_sym_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n;
    (void)off;
    (void)buf;
    (void)len;
    return -E_INVAL;
}

static const vfs_ops_t g_sysfs_sym_ops = {.read = sysfs_sym_read, .write = sysfs_dir_write};

static void fill_node(vfs_node_t *out, const char *name, int kind, const vfs_ops_t *ops, void *priv)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, name ? name : "?", VFS_NAME_MAX - 1);
    out->kind = kind;
    out->ops  = ops;
    out->priv = priv;
}

/* ---- resolve ------------------------------------------------------------- */

int sysfs_resolve(const char *path, vfs_node_t *out, int follow)
{
    if (!path || strncmp(path, "/sys", 4) != 0)
        return -E_INVAL;
    if (path[4] != '\0' && path[4] != '/')
        return -E_INVAL;

    sysfs_leaf_t leaf;
    int          r = sysfs_walk(path[4] ? path + 5 : "", &leaf);
    if (r)
        return r;

    switch (leaf.kind) {
    case LEAF_DIR:
        fill_node(out, leaf.name, VFS_DIR, &g_sysfs_dir_ops, NULL);
        return 0;
    case LEAF_ATTR:
        fill_node(out, leaf.name, VFS_FILE, &g_sysfs_attr_ops, leaf.attr);
        return 0;
    case LEAF_BIN:
        fill_node(out, leaf.name, VFS_FILE, &g_sysfs_bin_ops, leaf.bin);
        return 0;
    case LEAF_SYM:
        if (follow && leaf.sym->target) {
            /* Symlink targets are always kobjects: a followed link
             * resolves straight to the target directory. */
            fill_node(out, leaf.sym->target->name, VFS_DIR, &g_sysfs_dir_ops, NULL);
            return 0;
        }
        fill_node(out, leaf.name, VFS_SYMLINK, &g_sysfs_sym_ops, NULL);
        return 0;
    default:
        return -E_NOENT;
    }
}

/* ---- readlink ------------------------------------------------------------ */

int sysfs_readlink(const char *path, char *buf, uint32_t cap)
{
    if (!path || strncmp(path, "/sys", 4) != 0 || (path[4] != '\0' && path[4] != '/'))
        return -E_INVAL;

    sysfs_leaf_t leaf;
    int          r = sysfs_walk(path[4] ? path + 5 : "", &leaf);
    if (r)
        return r;
    if (leaf.kind != LEAF_SYM || !leaf.sym->target)
        return -E_INVAL;

    /* The link text is the tree-relative path from the link's directory to
     * its target -- the same "../..." spelling Linux sysfs uses, so libudev
     * can concatenate it onto the link's own path. */
    char *target = sysfs_relative_path(leaf.dir, leaf.sym->target);
    if (!target)
        return -E_NOENT;
    size_t len = strlen(target);
    if (cap && len > cap - 1)
        len = cap - 1;
    memcpy(buf, target, len);
    kfree(target);
    return (int)len;
}

/* ---- readdir ------------------------------------------------------------- */

/* Enumerate one kobject's directory: ".", "..", child kobjects, attribute
 * files, binary attribute files, symlinks -- index-addressed. */
static int sysfs_enum_kobj(struct kobject *kobj, uint32_t index, char *name, uint8_t *type)
{
    uint32_t n = 0;

    if (index == n++) {
        strncpy(name, ".", VFS_NAME_MAX - 1);
        *type = DT_DIR;
        return 0;
    }
    if (index == n++) {
        strncpy(name, "..", VFS_NAME_MAX - 1);
        *type = DT_DIR;
        return 0;
    }
    index -= n - 2;

    for (clist_t node = kobj->children; node; node = node->next, n++) {
        struct kobject *child = node->data;
        if (!child || !child->name) {
            n--;
            continue;
        }
        if (index == (n - 2)) {
            strncpy(name, child->name, VFS_NAME_MAX - 1);
            *type = DT_DIR;
            return 0;
        }
    }
    for (clist_t node = kobj->attributes; node; node = node->next, n++) {
        sysfs_attr_entry_t *e = node->data;
        if (!e || !e->attr || !e->attr->name) {
            n--;
            continue;
        }
        if (index == (n - 2)) {
            strncpy(name, e->attr->name, VFS_NAME_MAX - 1);
            *type = DT_REG;
            return 0;
        }
    }
    for (clist_t node = kobj->bin_attributes; node; node = node->next, n++) {
        sysfs_bin_attr_entry_t *e = node->data;
        if (!e || !e->attr || !e->attr->attr.name) {
            n--;
            continue;
        }
        if (index == (n - 2)) {
            strncpy(name, e->attr->attr.name, VFS_NAME_MAX - 1);
            *type = DT_REG;
            return 0;
        }
    }
    for (clist_t node = kobj->symlinks; node; node = node->next, n++) {
        sysfs_symlink_entry_t *e = node->data;
        if (!e || !e->name) {
            n--;
            continue;
        }
        if (index == (n - 2)) {
            strncpy(name, e->name, VFS_NAME_MAX - 1);
            *type = DT_LNK;
            return 0;
        }
    }
    return -E_NOENT;
}

int sysfs_readdir(const char *dirpath, uint32_t index, char *name, uint8_t *type)
{
    if (!dirpath || strncmp(dirpath, "/sys", 4) != 0 || (dirpath[4] != '\0' && dirpath[4] != '/'))
        return -E_NOTDIR;

    if (dirpath[4] == '\0' || dirpath[5] == '\0') {
        if (!sysfs_ensure_root()) {
            /* ready */
        } else {
            return -E_NOENT;
        }
        return sysfs_enum_kobj(sysfs_root_kobj, index, name, type);
    }

    sysfs_leaf_t leaf;
    int          r = sysfs_walk(dirpath + 5, &leaf);
    if (r)
        return r;
    if (leaf.kind == LEAF_SYM) {
        /* A directory listing through a class link reads the target. */
        if (!leaf.sym->target)
            return -E_NOENT;
        return sysfs_enum_kobj(leaf.sym->target, index, name, type);
    }
    if (leaf.kind != LEAF_DIR)
        return -E_NOTDIR;
    return sysfs_enum_kobj(leaf.kobj, index, name, type);
}

/* ---- create / remove ----------------------------------------------------- */

/*
 * Register an attribute on `dir_kobj`'s directory list.  `owner` is the
 * kobject whose ktype dispatches show/store -- the same as `dir_kobj` for
 * plain attributes, the group's parent for group attributes.
 */
static int sysfs_create_file_mode(struct kobject *dir_kobj, struct kobject *owner,
                                  const struct attribute *attr, uint16_t mode)
{
    if (!dir_kobj || !owner || !attr || !sysfs_name_valid(attr->name))
        return -E_INVAL;
    if (sysfs_name_exists(dir_kobj, attr->name))
        return -E_EXIST;

    sysfs_attr_entry_t *entry = kcalloc(1, sizeof(*entry));
    if (!entry)
        return -E_NOMEM;
    entry->attr  = (struct attribute *)attr;
    entry->owner = owner;
    entry->mode  = mode;

    int ret = kobject_list_add(&dir_kobj->attributes, entry);
    if (ret != 0) {
        kfree(entry);
        return ret;
    }
    return 0;
}

/* Create an attribute file. */
int sysfs_create_file(struct kobject *kobj, const struct attribute *attr)
{
    return sysfs_create_file_mode(kobj, kobj, attr, attr ? attr->mode : 0);
}

/* Remove an attribute file. */
void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr)
{
    if (!kobj || !attr || !attr->name)
        return;

    sysfs_attr_entry_t *entry = sysfs_find_attr(kobj, attr->name);
    if (!entry)
        return;

    kobj->attributes = clist_delete(kobj->attributes, entry);
    kfree(entry);
}

/* Create a binary attribute file with the given mode. */
static int sysfs_create_bin_file_mode(struct kobject *kobj, struct kobject *owner,
                                      const struct bin_attribute *attr, uint16_t mode)
{
    if (!kobj || !attr || !sysfs_name_valid(attr->attr.name))
        return -E_INVAL;
    if (sysfs_name_exists(kobj, attr->attr.name))
        return -E_EXIST;

    sysfs_bin_attr_entry_t *entry = kcalloc(1, sizeof(*entry));
    if (!entry)
        return -E_NOMEM;
    entry->attr  = (struct bin_attribute *)attr;
    entry->owner = owner;
    entry->mode  = mode;

    int ret = kobject_list_add(&kobj->bin_attributes, entry);
    if (ret != 0) {
        kfree(entry);
        return ret;
    }
    return 0;
}

/* Create a binary attribute file. */
int sysfs_create_bin_file(struct kobject *kobj, const struct bin_attribute *attr)
{
    return sysfs_create_bin_file_mode(kobj, kobj, attr, attr ? attr->attr.mode : 0);
}

/* Remove a binary attribute file. */
void sysfs_remove_bin_file(struct kobject *kobj, const struct bin_attribute *attr)
{
    if (!kobj || !attr || !attr->attr.name)
        return;

    sysfs_bin_attr_entry_t *entry = sysfs_find_bin_attr(kobj, attr->attr.name);
    if (!entry || entry->attr != attr)
        return;

    kobj->bin_attributes = clist_delete(kobj->bin_attributes, entry);
    kfree(entry);
}

/* Create a symlink under a kobject. */
int sysfs_create_symlink(struct kobject *kobj, struct kobject *target, const char *name)
{
    if (!kobj || !target || !sysfs_name_valid(name))
        return -E_INVAL;
    if (sysfs_name_exists(kobj, name))
        return -E_EXIST;

    sysfs_symlink_entry_t *entry = kcalloc(1, sizeof(*entry));
    if (!entry)
        return -E_NOMEM;
    entry->name = kstrdup(name);
    if (!entry->name) {
        kfree(entry);
        return -E_NOMEM;
    }
    entry->target = kobject_get(target);
    if (!entry->target) {
        kfree(entry->name);
        kfree(entry);
        return -E_NODEV;
    }

    int ret = kobject_list_add(&kobj->symlinks, entry);
    if (ret != 0) {
        kobject_put(entry->target);
        kfree(entry->name);
        kfree(entry);
        return ret;
    }
    return 0;
}

/* Remove a symlink under a kobject. */
void sysfs_remove_symlink(struct kobject *kobj, const char *name)
{
    if (!kobj || !name)
        return;

    sysfs_symlink_entry_t *entry = sysfs_find_symlink(kobj, name);
    if (!entry)
        return;

    kobj->symlinks = clist_delete(kobj->symlinks, entry);
    kobject_put(entry->target);
    kfree(entry->name);
    kfree(entry);
}

/* Create a group of attributes. */
int sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp)
{
    struct kobject        *target_kobj       = kobj;
    struct attribute     **created_attrs     = NULL;
    struct bin_attribute **created_bin_attrs = NULL;
    size_t                 attr_capacity     = 0;
    size_t                 bin_capacity      = 0;
    size_t                 attr_count        = 0;
    size_t                 bin_count         = 0;
    int                    ret;

    if (!kobj || !grp)
        return -E_INVAL;

    if (grp->attrs)
        while (grp->attrs[attr_capacity])
            attr_capacity++;
    if (grp->bin_attrs)
        while (grp->bin_attrs[bin_capacity])
            bin_capacity++;

    if (attr_capacity) {
        created_attrs = kcalloc(attr_capacity, sizeof(struct attribute *));
        if (!created_attrs)
            return -E_NOMEM;
    }
    if (bin_capacity) {
        created_bin_attrs = kcalloc(bin_capacity, sizeof(struct bin_attribute *));
        if (!created_bin_attrs) {
            kfree(created_attrs);
            return -E_NOMEM;
        }
    }

    /* If the group has a name, create a subdirectory */
    if (grp->name) {
        if (!sysfs_name_valid(grp->name)) {
            ret = -E_INVAL;
            goto err_group;
        }
        if (sysfs_name_exists(kobj, grp->name)) {
            ret = -E_EXIST;
            goto err_group;
        }
        target_kobj = kobject_create_and_add(grp->name, kobj);
        if (!target_kobj) {
            ret = -E_NOMEM;
            goto err_group;
        }
    }

    /* Create regular attribute files */
    if (grp->attrs) {
        struct attribute **attr;
        for (attr = grp->attrs; *attr; attr++) {
            if (!(*attr)->name)
                continue;

            uint16_t mode = (*attr)->mode;
            if (grp->is_visible) {
                int visible = grp->is_visible(kobj, *attr, (int)(attr - grp->attrs));
                if (!visible)
                    continue;
                mode = (uint16_t)visible;
            }

            ret = sysfs_create_file_mode(target_kobj, kobj, *attr, mode);
            if (ret != 0)
                goto err_group;
            created_attrs[attr_count++] = *attr;
        }
    }

    /* Create binary attribute files */
    if (grp->bin_attrs) {
        struct bin_attribute **bin;
        for (bin = grp->bin_attrs; *bin; bin++) {
            if (!(*bin)->attr.name)
                continue;

            uint16_t mode = (*bin)->attr.mode;
            if (grp->is_visible) {
                int visible = grp->is_visible(kobj, &(*bin)->attr, (int)(bin - grp->bin_attrs));
                if (!visible)
                    continue;
                mode = (uint16_t)visible;
            }

            ret = sysfs_create_bin_file_mode(target_kobj, kobj, *bin, mode);
            if (ret != 0)
                goto err_group;
            created_bin_attrs[bin_count++] = *bin;
        }
    }

    kfree(created_attrs);
    kfree(created_bin_attrs);
    return 0;
err_group:
    while (attr_count)
        sysfs_remove_file(target_kobj, created_attrs[--attr_count]);
    while (bin_count)
        sysfs_remove_bin_file(target_kobj, created_bin_attrs[--bin_count]);
    if (grp->name && target_kobj != kobj) {
        kobject_del(target_kobj);
        kobject_put(target_kobj);
    }
    kfree(created_attrs);
    kfree(created_bin_attrs);
    return ret;
}

/* Remove a group of attributes. */
void sysfs_remove_group(struct kobject *kobj, const struct attribute_group *grp)
{
    struct kobject *target_kobj;

    if (!kobj || !grp)
        return;

    /* If the group has a name, find the subdirectory kobject */
    if (grp->name) {
        target_kobj = kobject_find_child(kobj, grp->name);
        if (!target_kobj)
            return;
    } else {
        target_kobj = kobj;
    }

    if (grp->attrs) {
        struct attribute **attr;
        for (attr = grp->attrs; *attr; attr++) {
            if (!(*attr)->name)
                continue;
            sysfs_remove_file(target_kobj, *attr);
        }
    }
    if (grp->bin_attrs) {
        struct bin_attribute **bin;
        for (bin = grp->bin_attrs; *bin; bin++) {
            if (!(*bin)->attr.name)
                continue;
            sysfs_remove_bin_file(target_kobj, *bin);
        }
    }

    if (grp->name) {
        kobject_del(target_kobj);
        kobject_put(target_kobj);
    }
}

/* Create several attribute groups. */
int sysfs_create_groups(struct kobject *kobj, const struct attribute_group **groups)
{
    if (!groups)
        return 0;

    for (int i = 0; groups[i]; i++) {
        int ret = sysfs_create_group(kobj, groups[i]);
        if (ret != 0) {
            /* Rollback previously created groups */
            for (int j = i - 1; j >= 0; j--)
                sysfs_remove_group(kobj, groups[j]);
            return ret;
        }
    }
    return 0;
}

/* Remove several attribute groups. */
void sysfs_remove_groups(struct kobject *kobj, const struct attribute_group **groups)
{
    if (!groups)
        return;
    for (int i = 0; groups[i]; i++)
        sysfs_remove_group(kobj, groups[i]);
}

/* Remove every attribute, binary attribute and symlink of a kobject. */
void sysfs_cleanup_kobject_files(struct kobject *kobj)
{
    if (!kobj)
        return;

    while (kobj->attributes) {
        sysfs_attr_entry_t *entry = kobj->attributes->data;
        kobj->attributes          = clist_delete(kobj->attributes, entry);
        kfree(entry);
    }
    while (kobj->bin_attributes) {
        sysfs_bin_attr_entry_t *entry = kobj->bin_attributes->data;
        kobj->bin_attributes          = clist_delete(kobj->bin_attributes, entry);
        kfree(entry);
    }
    while (kobj->symlinks) {
        sysfs_symlink_entry_t *entry = kobj->symlinks->data;
        kobj->symlinks               = clist_delete(kobj->symlinks, entry);
        kobject_put(entry->target);
        kfree(entry->name);
        kfree(entry);
    }
}

/*
 * Mark a kobject visible under /sys.  The GNOS VFS materialises directories
 * on demand, so "creating the directory" is the name/collision validation
 * plus the visibility flag.  A deleted parent leaves registered children
 * unreachable from the root, which is exactly the Uinxed behaviour after
 * its sysfs_remove_dir() detaches the directory node.
 */
int sysfs_create_dir(struct kobject *kobj)
{
    if (!kobj)
        return -E_INVAL;
    if (kobj->state_in_sysfs)
        return -E_EXIST;
    if (!kobj->name || !kobj->name[0])
        return -E_INVAL;
    if (kobj->parent) {
        struct kobject *collision = kobject_find_child(kobj->parent, kobj->name);
        if ((collision && collision != kobj) || sysfs_find_attr(kobj->parent, kobj->name) ||
            sysfs_find_bin_attr(kobj->parent, kobj->name) ||
            sysfs_find_symlink(kobj->parent, kobj->name))
            return -E_EXIST;
    }
    kobj->state_in_sysfs = 1;
    return 0;
}

/* Mark a kobject's sysfs directory gone. */
void sysfs_remove_dir(struct kobject *kobj)
{
    if (!kobj || !kobj->state_in_sysfs)
        return;
    kobj->state_in_sysfs = 0;
}

/* Validate a rename against the parent directory's contents.  The name
 * itself is updated by kobject_rename(); there is no cached VFS node to
 * retitle. */
int sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
    if (!kobj || !new_name || !new_name[0])
        return -E_INVAL;
    if (kobj->parent) {
        struct kobject *collision = kobject_find_child(kobj->parent, new_name);
        if ((collision && collision != kobj) || sysfs_find_attr(kobj->parent, new_name) ||
            sysfs_find_bin_attr(kobj->parent, new_name) ||
            sysfs_find_symlink(kobj->parent, new_name))
            return -E_EXIST;
    }
    return 0;
}

/* Collision check for a kobject move; kobject_move() relinks the tree. */
int sysfs_move_dir(struct kobject *kobj, struct kobject *new_parent)
{
    if (!kobj || !new_parent)
        return -E_INVAL;
    struct kobject *collision = kobject_find_child(new_parent, kobj->name);
    if ((collision && collision != kobj) || sysfs_find_attr(new_parent, kobj->name) ||
        sysfs_find_bin_attr(new_parent, kobj->name) || sysfs_find_symlink(new_parent, kobj->name))
        return -E_EXIST;
    return 0;
}

/* ---- legacy registration ------------------------------------------------- */

int sysfs_add_file(const char *relpath, sysfs_gen_t gen, sysfs_put_t put)
{
    if (!relpath || !gen)
        return -E_INVAL;
    if (relpath[0] == '/' || !relpath[0])
        return -E_INVAL;
    if (strchr(relpath, '/') == relpath + strlen(relpath) - 1)
        return -E_INVAL;

    int ret = sysfs_ensure_root();
    if (ret)
        return ret;

    /* Find-or-create the directory chain from the root. */
    struct kobject *cur = sysfs_root_kobj;
    const char     *p   = relpath;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t      clen  = slash ? (size_t)(slash - p) : strlen(p);
        if (!clen)
            return -E_INVAL;
        if (clen >= KOBJ_NAME_LEN)
            return -E_NAMETOOLONG;

        char comp[KOBJ_NAME_LEN];
        memcpy(comp, p, clen);
        comp[clen] = '\0';

        struct kobject *child = kobject_find_child(cur, comp);
        if (!slash) {
            /* Leaf component: it names the attribute on `cur` (or on a
             * child kobject of the same name for two-level "dir/file"
             * paths like "kernel/ostype" -- the kobject "kernel" hosts
             * the attribute "ostype"). */
            sysfs_attr_entry_t *dup = sysfs_find_attr(cur, comp);
            if (dup)
                return -E_EXIST;

            legacy_ent_t *e = kcalloc(1, sizeof(*e));
            if (!e)
                return -E_NOMEM;
            e->gen       = gen;
            e->put       = put;
            e->attr.name = kstrdup(comp);
            e->attr.mode = (uint16_t)(put ? 0644 : 0444);
            if (!e->attr.name) {
                kfree(e);
                return -E_NOMEM;
            }
            ret = sysfs_create_file_mode(cur, cur, &e->attr, e->attr.mode);
            if (ret != 0) {
                kfree((void *)e->attr.name);
                kfree(e);
            }
            return ret;
        }

        if (!child) {
            /* Intermediate: needs a ktype that can dispatch the legacy
             * callbacks when this level later hosts a leaf attribute. */
            child = kcalloc(1, sizeof(struct kobject));
            if (!child)
                return -E_NOMEM;
            kobject_init(child, &legacy_ktype);
            ret = kobject_add(child, cur, "%s", comp);
            if (ret != 0) {
                kobject_put(child);
                return ret;
            }
        } else if (!child->ktype || !child->ktype->sysfs_ops) {
            /* Existing generic kobject: switch dispatch to the legacy ops.
             * The release hooks are compatible (both kfree the object). */
            child->ktype = &legacy_ktype;
        }
        cur = child;
        p   = slash + 1;
    }
}

/* Boot-time initialisation: the kobject root plus built-in attributes. */
void sysfs_init(void)
{
    (void)sysfs_ensure_root();
}

#!/bin/sh
# build-alpine-rootfs.sh — make GNOS's userland be Alpine Linux.  (GPLv2)
#
# The image used to be assembled one .apk at a time by install-alpine.sh,
# which meant the guest had Alpine's *binaries* but none of what actually
# makes a system: the skeleton in /etc (os-release, passwd, shadow, profile,
# inittab), the package manager's own state in /lib/apk and the trust store
# /etc/ssl/certs.  Those were re-invented by hand in src/rootfs — and the
# copy of /etc/os-release there is exactly the "GNOS pretends to be its own
# distribution" habit this script replaces.
#
# So: take Alpine's own minirootfs, which IS a root filesystem (it is what
# every Alpine container, chroot and diskless boot starts from), and let apk
# itself install the extra packages into it.  Everything Alpine ships then
# comes from Alpine, including:
#   /etc/os-release                     NAME="Alpine Linux", ID=alpine
#   /etc/{passwd,group,shadow,profile,inittab,fstab,hostname}
#   /sbin/apk + /lib/apk/db             a working package manager
#   /lib/libssl.so.3 /lib/libcrypto.so.3 /lib/libz.so.1
#   /etc/apk/keys/*.rsa.pub             the signatures apk will trust
#   /etc/ssl/certs/ca-certificates.crt  the CA bundle its HTTPS needs
#   /etc/apk/repositories               already https://
#   /bin/busybox with every applet link already made.
#
# apk runs `--root` rather than in a chroot: it needs no privileges that way,
# and --no-scripts keeps none of the packages' maintainer code running on the
# host.  The one trigger worth reproducing -- the bitmap-font index Xorg wants
# (mkfontscale's job) -- is done at the end with the host's own tools.
#
# Usage:
#   tools/build-alpine-rootfs.sh
# Env overrides:
#   ALPINE_BRANCH   (v3.20)        ALPINE_ARCH     (x86_64)
#   ALPINE_MIRROR   (https://dl-cdn.alpinelinux.org/alpine)
#   ALPINE_PKGS     (extra packages on top of the minirootfs)
#   GNOS_BUILD      (build/)
set -eu

BRANCH=${ALPINE_BRANCH:-v3.20}
ARCH=${ALPINE_ARCH:-x86_64}
MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${GNOS_BUILD:-$HERE/build}
ROOTFS=$BUILD/alpine-rootfs
CACHE=$BUILD/apkcache

# Everything the minirootfs does not carry.  It already has musl, busybox
# (with its applet links), alpine-baselayout, alpine-keys, apk-tools,
# ca-certificates-bundle and zlib/openssl, so only the parts GNOS actually
# uses on top of that are listed.  musl-dev is not for the guest: it is the
# sysroot src/usr's own programs are compiled and linked against
# (usr/include + usr/lib/{libc.a,crt*.o}).
PKGS=${ALPINE_PKGS:-"musl-dev openrc bash coreutils curl nano python3 \
                     fastfetch ncurses-terminfo \
                     file util-linux-login linux-pam \
                     xorg-server xf86-input-evdev xkbcomp xkeyboard-config \
                     xinit \
                     mesa mesa-dri-gallium \
                     twm xterm xeyes xsetroot mkfontscale \
                     agetty dbus xfce4 xfce4-terminal"}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fetch() {                       # url, dest
    if [ ! -s "$2" ]; then
        echo "  fetch $1"
        curl -fsSL --retry 3 "$1" -o "$2.part" && mv "$2.part" "$2"
    fi
}

mkdir -p "$CACHE"
echo "=== Alpine $BRANCH ($ARCH) minirootfs -> $ROOTFS ==="

# ---- 1. the newest minirootfs of this branch ----------------------------
# The filename carries the point release (3.20.10), which only the directory
# listing knows, so ask for the listing and take the highest one.
REL_DIR="$MIRROR/$BRANCH/releases/$ARCH/"
MINI=$(curl -fsSL "$REL_DIR" |
       grep -o "alpine-minirootfs-[0-9.]*-$ARCH\.tar\.gz" | sort -Vu | tail -1)
[ -n "$MINI" ] || { echo "build-alpine-rootfs: no minirootfs listed at $REL_DIR" >&2; exit 1; }
echo "  minirootfs: $MINI"
TARBALL="$CACHE/$MINI"
fetch "$REL_DIR$MINI" "$TARBALL"

# ---- 2. a static apk to install into it ---------------------------------
# The host has no apk of its own, and the one inside the minirootfs is a
# musl dynamic binary the host cannot simply run.  apk-tools-static is the
# answer: one file, no dependencies, and --root needs no privileges at all.
APK_STATIC=$CACHE/apk.static
if [ ! -x "$APK_STATIC" ]; then
    INDEX_TGZ=$CACHE/APKINDEX-$BRANCH-main.tar.gz
    fetch "$MIRROR/$BRANCH/main/$ARCH/APKINDEX.tar.gz" "$INDEX_TGZ"
    tar -xzOf "$INDEX_TGZ" APKINDEX > "$WORK/APKINDEX"
    V=$(awk '/^P:apk-tools-static$/{f=1;next} /^V:/{if(f){print substr($0,3);exit}}' \
        "$WORK/APKINDEX")
    [ -n "$V" ] || { echo "build-alpine-rootfs: no apk-tools-static in the index" >&2; exit 1; }
    fetch "$MIRROR/$BRANCH/main/$ARCH/apk-tools-static-$V.apk" \
          "$CACHE/apk-tools-static-$V.apk"
    tar -xzOf "$CACHE/apk-tools-static-$V.apk" sbin/apk.static > "$APK_STATIC"
    chmod +x "$APK_STATIC"
fi

# ---- 3. unpack the root filesystem --------------------------------------
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"
tar -xzf "$TARBALL" -C "$ROOTFS"

# ---- 4. install the extra packages with apk itself ----------------------
# --root does the extraction relative to that directory; --no-scripts keeps
# package maintainer code off the host.  The repository list and the signing
# keys both come from the minirootfs, and its repositories are already https.
"$APK_STATIC" --root "$ROOTFS" \
    --keys-dir "$ROOTFS/etc/apk/keys" \
    --repositories-file "$ROOTFS/etc/apk/repositories" \
    --arch "$ARCH" add --no-scripts $PKGS

# ---- 5. font index ------------------------------------------------------
# mkfontscale is normally run by a trigger that fires when files land in a
# font directory; --no-scripts means it never did.  Without fonts.dir and
# encodings.dir Xorg starts with an empty font path and xterm/twm come up
# blank.  -b is what makes the .pcf bitmap fonts visible, and the two -e
# directories are what fills encodings.dir.  mkfontscale echoes every XLFD it
# writes, hence stdout is dropped.
if [ -d "$ROOTFS/usr/share/fonts" ]; then
    find "$ROOTFS/usr/share/fonts" -mindepth 1 -maxdepth 1 -type d |
    while read -r fdir; do
        case "$fdir" in
            */encodings) continue ;;
        esac
        mkfontdir "$fdir" >/dev/null 2>&1 || true
        mkfontscale -b -e "$ROOTFS/usr/share/fonts/encodings" \
                    -e "$ROOTFS/usr/share/fonts/encodings/large" \
                    "$fdir" >/dev/null 2>&1 || true
        # The paths just written are host paths; point them at the image's own
        # font tree.  The encoding name comes first on the line, so this must
        # not be anchored.
        if [ -f "$fdir/encodings.dir" ]; then
            sed -i "s|$ROOTFS/usr/share/fonts/encodings|/usr/share/fonts/encodings|g" \
                "$fdir/encodings.dir"
        fi
    done
fi

# apk --root skips package triggers, so three data files the GTK stack needs
# are missing from a fresh tree:
#   shared-mime-info globs/globs2  (gdk-pixbuf picks a loader by mime type)
#   gdk-pixbuf loaders.cache       (maps a mime type to the loader .so)
#   GSettings gschemas.compiled    (compiled schema defaults)
# Without them every image decode fails ("Unrecognized image file format"),
# xfce4-session aborts at startup and all Xfce clients stall before mapping
# a window.  chroot needs root; bwrap runs the tree's own musl binaries
# unprivileged.  A writable --bind on the target directory is needed because
# the read-only root is mounted on top of it.
if command -v bwrap >/dev/null 2>&1; then
    HAVE_BWRAP=1
else
    HAVE_BWRAP=0
fi

MIME_DIR="$ROOTFS/usr/share/mime"
if [ -d "$MIME_DIR/packages" ] && [ ! -f "$MIME_DIR/globs" ]; then
    if [ "$HAVE_BWRAP" = 1 ]; then
        bwrap --ro-bind "$ROOTFS" / --dev /dev \
              --bind "$MIME_DIR" /usr/share/mime \
              /usr/bin/update-mime-database /usr/share/mime >/dev/null 2>&1 || true
    elif [ "$(id -u)" = 0 ]; then
        chroot "$ROOTFS" /usr/bin/update-mime-database /usr/share/mime >/dev/null 2>&1 || true
    fi
fi

LOADER_DIR="$ROOTFS/usr/lib/gdk-pixbuf-2.0/2.10.0"
if [ -x "$ROOTFS/usr/bin/gdk-pixbuf-query-loaders" ] && [ ! -f "$LOADER_DIR/loaders.cache" ]; then
    mkdir -p "$LOADER_DIR"
    if [ "$HAVE_BWRAP" = 1 ]; then
        bwrap --ro-bind "$ROOTFS" / --dev /dev --bind /tmp /tmp \
              /usr/bin/gdk-pixbuf-query-loaders > "$WORK/loaders.cache" 2>/dev/null &&
            cp "$WORK/loaders.cache" "$LOADER_DIR/loaders.cache" || true
    elif [ "$(id -u)" = 0 ]; then
        chroot "$ROOTFS" /usr/bin/gdk-pixbuf-query-loaders --update-cache 2>/dev/null || true
    fi
    [ -f "$LOADER_DIR/loaders.cache" ] ||
        echo "=== warning: loaders.cache not generated; xfce will abort on icons" >&2
fi

SCHEMA_DIR="$ROOTFS/usr/share/glib-2.0/schemas"
if [ -d "$SCHEMA_DIR" ] && [ ! -f "$SCHEMA_DIR/gschemas.compiled" ]; then
    if [ "$HAVE_BWRAP" = 1 ]; then
        bwrap --ro-bind "$ROOTFS" / --dev /dev \
              --bind "$SCHEMA_DIR" /usr/share/glib-2.0/schemas \
              /usr/bin/glib-compile-schemas /usr/share/glib-2.0/schemas >/dev/null 2>&1 || true
    elif [ "$(id -u)" = 0 ]; then
        chroot "$ROOTFS" /usr/bin/glib-compile-schemas /usr/share/glib-2.0/schemas/ >/dev/null 2>&1 || true
    fi
fi

# ---- xkbcomp diagnostic wrapper (TEMPORARY) ----------------------------
# On GNOS, Xorg invokes xkbcomp to compile the keyboard keymap and it exits
# non-zero with no visible reason, which fatals the server (Fatal server
# error: Failed to activate virtual core keyboard) and stops Xfce from
# connecting.  Swap in a wrapper that proxies to the real binary and appends
# its stderr to /tmp/xkbcomp.err (a tmpfs, so it is writable even if the
# root were read-only) so the next boot exposes the true error.  Delete this
# block once the underlying cause is fixed and the wrapper is no longer
# needed.
if [ -x "$ROOTFS/usr/bin/xkbcomp" ]; then
    mv "$ROOTFS/usr/bin/xkbcomp" "$ROOTFS/usr/bin/xkbcomp.real"
    cat > "$ROOTFS/usr/bin/xkbcomp" <<'XKBCOMP_WRAPPER'
#!/bin/sh
# xkbcomp diagnostic wrapper -- proxies to xkbcomp.real, logs stderr.
REAL=/usr/bin/xkbcomp.real
LOG=/tmp/xkbcomp.err
{
    echo "=== xkbcomp wrapper ($(date -u 2>/dev/null || echo 'no-date')) ==="
    echo "pid=$$ args: $@"
} >> "$LOG" 2>/dev/null
"$REAL" "$@" 2>> "$LOG"
ec=$?
echo "xkbcomp exit=$ec" >> "$LOG" 2>/dev/null
# Push the captured error to the debug console (serial) so it shows up in the
# captured build/dbg.log even when the login prompt is unreachable.
if [ -x /bin/dbgcat.elf ]; then
    /bin/dbgcat.elf "$LOG"
fi
exit $ec
XKBCOMP_WRAPPER
    chmod +x "$ROOTFS/usr/bin/xkbcomp"
    echo "=== xkbcomp diagnostic wrapper installed (logs to /tmp/xkbcomp.err) ==="
fi

echo "=== Alpine rootfs ready at $ROOTFS ==="
du -sh "$ROOTFS"

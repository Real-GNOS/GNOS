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
                     xorg-server xf86-input-evdev xkbcomp xkeyboard-config \
                     twm xterm xeyes xsetroot mkfontscale"}

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

echo "=== Alpine rootfs ready at $ROOTFS ==="
du -sh "$ROOTFS"

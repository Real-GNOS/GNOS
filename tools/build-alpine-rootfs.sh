#!/bin/sh
# build-alpine-rootfs.sh -- build a complete Alpine rootfs for GNOS.  (GPLv2)
#
# Modeled after Unixed-Kernel's tools/build-alpine-xfce-rootfs.sh: downloads
# Alpine minirootfs, installs a curated package set via apk, then overlays
# GNOS-specific configuration.  The resulting tree lives in
# build/alpine-rootfs/ and the Makefile folds it into the initrd.
#
# Dependencies: curl, tar, and (optionally) bwrap for full isolation.
# When bwrap is unavailable the script falls back to chroot or plain
# extraction -- apk --no-scripts never executes host code anyway.
#
# Usage:
#   tools/build-alpine-rootfs.sh
# Env overrides:
#   ALPINE_BRANCH  (default: v3.20)
#   ALPINE_ARCH    (default: x86_64)
#   GNOS_BUILD     (default: build/)
set -eu

BRANCH=${ALPINE_BRANCH:-v3.20}
ARCH=${ALPINE_ARCH:-x86_64}
MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${GNOS_BUILD:-$HERE/build}
ROOTFS=$BUILD/alpine-rootfs
CACHE=$BUILD/apkcache
MINIROOTFS="alpine-minirootfs-${BRANCH#v}.${ARCH}.tar.gz"

# The package set: base system plus GNOS desktop essentials.  Keep this
# list focused -- every package drags dependencies into the tarball.
PKGS="alpine-base openrc busybox-openrc \
       eudev eudev-openrc eudev-hwids udev-init-scripts-openrc \
       dbus dbus-openrc \
       bash coreutils util-linux shadow sudo \
       nano curl git gcc g++ make cmake \
       linux-headers ncurses-dev musl-dev \
       python3 py3-pip go rust cargo \
       htop tree file less which diffutils patch sed gawk \
       ncurses-terminfo ncurses-terminfo-base \
       terminfo-font"

mkdir -p "$CACHE"

fetch() {
    if [ ! -s "$2" ]; then
        echo "  fetch $1"
        curl -fsSL --retry 4 --output "$2.part" "$1"
        mv "$2.part" "$2"
    fi
}

echo "=== Alpine $BRANCH ($ARCH) -> $ROOTFS ==="

# ---- 1. download and extract minirootfs ----------------------------------
TARBALL="$CACHE/$MINIROOTFS"
fetch "$MIRROR/$BRANCH/releases/$ARCH/$MINIROOTFS" "$TARBALL"

rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"
tar -xzf "$TARBALL" -C "$ROOTFS"

# ---- 2. bootstrap apk inside the rootfs ----------------------------------
# Create the APK keys directory and populate from the host if available.
mkdir -p "$ROOTFS/etc/apk/keys"
if [ -d /etc/apk/keys ]; then
    cp /etc/apk/keys/* "$ROOTFS/etc/apk/keys/" 2>/dev/null || true
fi

# Set up the repositories file.
cat > "$ROOTFS/etc/apk/repositories" <<EOF
${MIRROR}/${BRANCH}/main
${MIRROR}/${BRANCH}/community
EOF

# ---- 3. install packages --------------------------------------------------
# Try bwrap first (Unixed-Kernel style), fall back to chroot, then to
# plain apk --root (which does not run scripts at all).
apk_install() {
    if command -v bwrap >/dev/null 2>&1; then
        echo "  installing via bwrap..."
        bwrap --unshare-user --uid 0 --gid 0 --unshare-pid --unshare-uts \
              --unshare-ipc --bind "$ROOTFS" / --proc /proc --dev /dev \
              --ro-bind /etc/resolv.conf /etc/resolv.conf \
              /sbin/apk add --no-scripts --usermode $PKGS
    elif command -v chroot >/dev/null 2>&1; then
        echo "  installing via chroot..."
        chroot "$ROOTFS" /sbin/apk add --no-scripts $PKGS
    else
        echo "  installing via apk --root (no script execution)..."
        # apk --root does not require a working chroot; it extracts
        # payloads directly, which is exactly what we want.
        /sbin/apk --root "$ROOTFS" --keys-dir "$ROOTFS/etc/apk/keys" \
                  --repositories-file "$ROOTFS/etc/apk/repositories" \
                  add --no-scripts $PKGS
    fi
}

apk_install

# ---- 4. busybox symlinks --------------------------------------------------
# Ensure BusyBox applets are available as individual commands.
if [ -x "$ROOTFS/bin/busybox" ]; then
    chroot "$ROOTFS" /bin/busybox --install -s 2>/dev/null || true
fi

# ---- 5. create messagebus user/group (needed by dbus) ---------------------
if [ -f "$ROOTFS/etc/passwd" ]; then
    if ! grep -q '^messagebus:' "$ROOTFS/etc/group" 2>/dev/null; then
        echo 'messagebus:x:103:' >> "$ROOTFS/etc/group"
    fi
    if ! grep -q '^messagebus:' "$ROOTFS/etc/passwd" 2>/dev/null; then
        echo 'messagebus:x:103:103:messagebus:/dev/null:/sbin/nologin' >> "$ROOTFS/etc/passwd"
    fi
fi

# ---- 6. OpenRC service enablement -----------------------------------------
# Register the services we want at boot.  This is the Unixed-Kernel pattern.
if [ -d "$ROOTFS/etc/runlevels" ]; then
    mkdir -p "$ROOTFS/etc/runlevels/sysinit"
    mkdir -p "$ROOTFS/etc/runlevels/default"
    mkdir -p "$ROOTFS/etc/runlevels/boot"
    for svc in udev udev-trigger udev-settle; do
        [ -f "$ROOTFS/etc/init.d/$svc" ] && \
            ln -sf /etc/init.d/$svc "$ROOTFS/etc/runlevels/sysinit/$svc" 2>/dev/null || true
    done
    for svc in udev-postmount dbus; do
        [ -f "$ROOTFS/etc/init.d/$svc" ] && \
            ln -sf /etc/init.d/$svc "$ROOTFS/etc/runlevels/default/$svc" 2>/dev/null || true
    done
    # devfs is not needed: the kernel provides /dev directly.
    rm -f "$ROOTFS/etc/runlevels/sysinit/devfs" 2>/dev/null || true
fi

# ---- 7. create essential directories --------------------------------------
mkdir -p "$ROOTFS/run/user/0" "$ROOTFS/var/lib/dbus" "$ROOTFS/var/log" 2>/dev/null || true

echo "=== Alpine rootfs ready at $ROOTFS ==="
echo "    packages: $PKGS"
du -sh "$ROOTFS"

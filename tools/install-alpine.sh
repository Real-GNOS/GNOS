#!/bin/sh
# install-alpine.sh — autoinstall Alpine Linux packages into the GNOS image.
# (GPLv2)
#
# Alpine ships prebuilt, musl-linked x86-64 binaries whose ABI is exactly
# what GNOS speaks (this kernel exists to run musl userland), so instead of
# cross-compiling each tool from source like build-fastfetch.sh does, this
# pulls the .apk packages straight from dl-cdn.alpinelinux.org, resolves
# their dependency tree against the repository index and unpacks them into
# build/alpine-root.  The Makefile folds that directory into the initrd.
#
# What it does NOT do:
#   - run maintainer scripts (.pre-install/.post-install): there is no shell
#     environment to run them in, and most payload-only tools do not need
#     them.  The one that matters -- busybox's applet links -- is reproduced
#     by hand further down;
#   - satisfy "so:lib..." virtual dependencies from the full package graph:
#     a package that needs a library the index cannot name as a plain
#     package is reported and skipped rather than guessed at.
#
# Usage:
#   tools/install-alpine.sh PKG [PKG ...]
# Env overrides: ALPINE_BRANCH (v3.20), ALPINE_REPO (main),
#                ALPINE_ARCH (x86_64), GNOS_BUILD (build dir).
set -e

BRANCH=${ALPINE_BRANCH:-v3.20}
# Packages are spread over more than one repository (xorg-server lives in
# community, its dependencies in main), so the whole set is searched at once.
REPOS=${ALPINE_REPOS:-${ALPINE_REPO:-main}}
ARCH=${ALPINE_ARCH:-x86_64}
MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${GNOS_BUILD:-$HERE/build}
ROOT=$BUILD/alpine-root
CACHE=$BUILD/apkcache
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ "$#" -eq 0 ]; then
    echo "usage: $0 PKG [PKG ...]  (env: ALPINE_BRANCH/REPO/ARCH)" >&2
    exit 2
fi

fetch() { # url, dest
    if [ ! -s "$2" ]; then
        echo "  fetch $1"
        curl -fsSL "$1" -o "$2" || wget -q "$1" -O "$2" \
            || { echo "install-alpine: cannot fetch $1 (network down?)" >&2
                 exit 1; }
    fi
}

echo "alpine $BRANCH/$REPO ($ARCH) -> $ROOT"
mkdir -p "$CACHE" "$ROOT"

# ---- repository indexes --------------------------------------------------
for REPO in $REPOS; do
    INDEX_TGZ=$CACHE/APKINDEX-$BRANCH-$REPO.tar.gz
    fetch "$MIRROR/$BRANCH/$REPO/$ARCH/APKINDEX.tar.gz" "$INDEX_TGZ"
    mkdir -p "$WORK/$REPO"
    tar -xzf "$INDEX_TGZ" -C "$WORK/$REPO"
    [ -f "$WORK/$REPO/APKINDEX" ] || { echo "install-alpine: APKINDEX missing for $REPO" >&2; exit 1; }
done

# ---- index queries -------------------------------------------------------
# A stanza is a run of lines up to a blank line; fields are "X:value".
# Indexes are per repository, so every lookup below takes a repo name and
# matches against that repo's copy of the index.
latest_of() { # name -> "<repo> <version>" of the newest match anywhere
    for repo in $REPOS; do
        awk -v want="$1" '
            /^P:/  { pkg=substr($0,3) }
            /^V:/  { ver=substr($0,3) }
            /^$/   { if (pkg==want) print ver; pkg=""; ver="" }
            END    { if (pkg==want) print ver }
        ' "$WORK/$repo/APKINDEX" | sort -Vu | while read -r ver; do
            printf '%s %s\n' "$repo" "$ver"
        done
    done | sort -k2,2V | tail -1
}

deps_of() { # repo name -> depend lines, one per token
    awk -v want="$2" '
        /^P:/  { pkg=substr($0,3) }
        /^D:/  { deps=substr($0,3) }
        /^$/   { if (pkg==want) print deps; pkg=""; deps="" }
        END    { if (pkg==want) print deps }
    ' "$WORK/$1/APKINDEX" | tr ' ' '\n' | sed '/^$/d'
}

have_pkg() { # name -> 0/1
    latest_of "$1" | grep -q .
}

# Some dependencies are virtual names (e.g. "pkgconfig") that no package
# declares as its own name -- they appear in the providing package's p: line.
# The same lookup resolves so:/cmd: dependencies ("so:libdrm.so.2") to the
# package that ships the file.
provider_of() { # virtual -> "<package>" or empty
    for repo in $REPOS; do
        awk -v want="$1" '
            /^P:/ { pkg=substr($0,3) }
            /^p:/ {
                n = split(substr($0,3), a, " ")
                for (i = 1; i <= n; i++) {
                    split(a[i], b, "=")
                    if (b[1] == want) { print pkg; exit }
                }
            }
        ' "$WORK/$repo/APKINDEX" | head -1
    done | head -1
}

# Strip a version operator off a dependency token, keep only real packages
# (ignore so:/pc:/cmd: virtuals and /path-style providers such as /bin/sh,
# which need the full provides graph).
bare_name() {
    case "$1" in
        so:*|pc:*|cmd:*|/*) return 1 ;;
        *) n=${1%%[<>=!]*}; n=${n%%:*}; [ -n "$n" ] || return 1
           printf '%s' "$n"; return 0 ;;
    esac
}

# ---- resolution ----------------------------------------------------------
# BFS over the dependency tree; versions are pinned to the index's latest.
resolved=""        # "name version repo" triples, one per line
seen=""
queue="$*"

while [ -n "$queue" ]; do
    want=$(echo "$queue" | awk '{print $1; exit}')
    queue=$(echo "$queue" | sed 's/^[^ ]* *//')
    case " $seen " in *" $want "*) continue ;; esac
    seen="$seen $want"

    pick=$(latest_of "$want")
    if [ -z "$pick" ]; then
        # Not a real package name: try to find something that provides it.
        prov=$(provider_of "$want")
        if [ -n "$prov" ]; then
            want=$prov
            pick=$(latest_of "$want")
        fi
    fi
    if [ -z "$pick" ]; then
        echo "install-alpine: package '$want' not found in $BRANCH/$REPOS" >&2
        exit 1
    fi
    repo=${pick%% *}
    ver=${pick#* }
    resolved="$resolved
$want $ver $repo"

    for dep in $(deps_of "$repo" "$want"); do
        child=""
        child=$(bare_name "$dep") || child=""
        if [ -z "$child" ]; then
            # so:/pc:/cmd: dependencies name a file or command, not a
            # package; the package that provides it has to be looked up.
            base=${dep%%[<>=!]*}
            child=$(provider_of "$base") || child=""
        fi
        [ -n "$child" ] || continue
        case " $seen " in *" $child "*) continue ;; esac
        queue="$queue $child"
    done
done

# ---- download + unpack ---------------------------------------------------
echo "resolved:"
echo "$resolved" | sed '/^$/d' | awk '{printf "  %s-%s\n", $1, $2}'
echo "$resolved" | sed '/^$/d' | while read -r name ver repo; do
    apk="$CACHE/$name-$ver.apk"
    fetch "$MIRROR/$BRANCH/$repo/$ARCH/$name-$ver.apk" "$apk"
    # Control files sit at the top of the archive; exclude them so they do
    # not land in the filesystem root.  apk stores them as ".PKGINFO" with
    # no leading "./" -- tar matches --exclude patterns verbatim, so the
    # "./" prefix never matched anything and these leaked into the tree.
    # Payload extraction is additive, so re-running with more packages
    # merges cleanly.
    tar -xzf "$apk" -C "$ROOT" \
        --exclude='.PKGINFO' --exclude='.SIGN*' \
        --exclude='.pre-install' --exclude='.post-install' \
        --exclude='.pre-upgrade' --exclude='.post-upgrade' \
        --exclude='.trigger' --exclude='.installed' \
        --exclude='.commit' 2>/dev/null || {
        # fall back for repos that ship zstd archives
        tar --zstd -xf "$apk" -C "$ROOT" \
            --exclude='.PKGINFO' --exclude='.SIGN*' \
            --exclude='.pre-install' --exclude='.post-install' \
            --exclude='.pre-upgrade' --exclude='.post-upgrade' \
            --exclude='.trigger' --exclude='.installed' \
            --exclude='.commit' 2>/dev/null \
            || { echo "install-alpine: cannot unpack $name-$ver.apk" >&2
                 exit 1; }
    }
done

# ---- busybox applet links -------------------------------------------------
# Alpine's busybox package ships /bin/busybox and nothing else: the 304
# applet links come from its .post-install, which is one of the maintainer
# scripts this tool deliberately does not run -- there is no apk machinery
# here to run them.  Skip them and the image has no grep, sed, awk, hostname,
# ifconfig, ping, wget, find, ps ... so /etc/rc and every OpenRC service
# script dies on its first pipeline.
#
# Reproduce that script's work.  It runs `busybox --install -s` with no DIR,
# which writes each applet to its compiled-in path (bin/, sbin/, usr/bin/,
# usr/sbin/); --list-full prints exactly those paths, already relative to the
# root, so no chroot and no root privileges are needed.  busybox is linked
# against musl and cannot run directly on a glibc host, so invoke it through
# the loader staged beside it.  A name another package already provides is
# left alone: apk lets the payload win, and so do we.
if [ -x "$ROOT/bin/busybox" ] && [ -e "$ROOT/lib/ld-musl-x86_64.so.1" ]; then
    if list=$("$ROOT/lib/ld-musl-x86_64.so.1" "$ROOT/bin/busybox" \
              --list-full 2>/dev/null) && [ -n "$list" ]; then
        n=0
        for rel in $list; do
            case "$rel" in
                bin/*|sbin/*|usr/bin/*|usr/sbin/*) ;;
                *) continue ;;
            esac
            if [ ! -e "$ROOT/$rel" ]; then
                mkdir -p "$(dirname "$ROOT/$rel")"
                # absolute, exactly like busybox's own installer: /bin/ash
                # and usr/bin/awk must both resolve to /bin/busybox.
                ln -sf /bin/busybox "$ROOT/$rel"
                n=$((n + 1))
            fi
        done
        echo "busybox: $n applet links"
    fi
fi

# ---- font index ----------------------------------------------------------
# Alpine's mkfontscale package builds the per-directory font index from a
# .trigger that fires when files land in a font directory:
#
#     for i in "$@"; do case "$i" in */encodings) continue;; esac
#                         mkfontdir "$i"; mkfontscale "$i"; done
#
# We unpack without running triggers, so fonts.dir and encodings.dir are
# absent: Xorg then sees an empty font path and xterm/twm start blank.  Do
# the trigger's work here with the host's tools.  -b is what makes the .pcf
# bitmap fonts visible (the default is to ignore them), and the two -e dirs
# are what fill encodings.dir, which X reads out of the font directory to
# decode XLFD charset/registry names.  mkfontscale also prints every XLFD it
# writes, hence stdout is dropped.
if [ -d "$ROOT/usr/share/fonts" ]; then
    echo "indexing fonts"
    find "$ROOT/usr/share/fonts" -mindepth 1 -maxdepth 1 -type d |
    while read -r fdir; do
        case "$fdir" in
            */encodings) continue ;;   # encodings dir holds no fonts
        esac
        mkfontdir "$fdir" >/dev/null 2>&1 || true
        mkfontscale -b \
            -e "$ROOT/usr/share/fonts/encodings" \
            -e "$ROOT/usr/share/fonts/encodings/large" \
            "$fdir" >/dev/null 2>&1 || true
        # The paths just written are host paths; drop the staging prefix so
        # they resolve to the image's own /usr/share/fonts/encodings/*.enc.gz.
        # The path is not at the start of the line (the encoding name is),
        # so this must not be anchored.
        if [ -f "$fdir/encodings.dir" ]; then
            sed -i "s|$ROOT/usr/share/fonts/encodings|/usr/share/fonts/encodings|g" \
                "$fdir/encodings.dir"
        fi
    done
fi

echo "done: contents staged in $ROOT (make will fold it into the initrd)"

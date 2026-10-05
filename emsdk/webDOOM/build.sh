#!/bin/bash
#
# build.sh -- build webDOOM with emscripten and emit browser-runnable
# packages (html/js/wasm/data) into build/web/.
#
# usage:  ./build.sh [doom1|doom2|all] [--clean]
#
#   doom1|doom2   which IWAD to bundle (default: doom1)
#   all           build both packages
#   --clean       wipe every object file first (full rebuild)
#
# The final link is done by overriding make's LINK variable: that reuses the
# exact object list and LDADD/LIBS the Makefiles computed, and lets us attach
# --preload-file (the .data package), -O3 and the memory/legacy-GL settings.
#

set -e
cd "$(dirname "$0")"

games="doom1"
clean=0
for arg in "$@"; do
    case "$arg" in
        doom1|1)     games="doom1" ;;
        doom2|2)     games="doom2" ;;
        all|both)    games="doom1 doom2" ;;
        --clean|-c)  clean=1 ;;
        -h|--help)   sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "build.sh: unknown argument '$arg' (try --help)" >&2; exit 1 ;;
    esac
done

# The compiler has to be the one the Makefiles were configured with, so read
# it back out of src/Makefile instead of trusting $PATH.
EMCC=$(sed -n 's/^CC = //p' src/Makefile | head -1)
if [ -z "$EMCC" ] || [ ! -x "$EMCC" ]; then
    echo "build.sh: cannot find emcc (checked '^CC =' in src/Makefile)" >&2
    exit 1
fi

for g in $games; do
    for f in "build/prboom.wad" "build/$g.wad" "build/$g/music" "build/sfx"; do
        if [ ! -e "$f" ]; then
            echo "build.sh: missing asset '$f' -- run extract_assets.py first" >&2
            exit 1
        fi
    done
done

if [ "$clean" = 1 ]; then
    make clean
fi

mkdir -p build/web

# games_PROGRAMS=prboom keeps prboom-game-server (a server binary, useless in a
# browser) out of the build.  Command-line variables propagate to sub-Makefiles
# that neither of them is used by, so this is safe for the whole tree.
for g in $games; do
    rm -f "build/web/$g.html" "build/web/$g.js" \
          "build/web/$g.wasm" "build/web/$g.data"

    # Drop the current binary so make always re-runs the link recipe below;
    # the LINK override is what carries the preloaded data package into the
    # output.  Nothing else in the tree needs rebuilding between games.
    rm -f src/prboom src/prboom.wasm

    make games_PROGRAMS=prboom LINK="$EMCC -O3 \
        -sUSE_SDL=1 \
        -sLEGACY_GL_EMULATION=1 \
        -sINITIAL_MEMORY=256MB \
        --preload-file ../build/prboom.wad@/prboom.wad \
        --preload-file ../build/$g.wad@/$g.wad \
        --preload-file ../build/$g/music@/$g/music \
        --preload-file ../build/sfx@/sfx \
        -o ../build/web/$g.html"
done

echo
echo "built in build/web/:"
for g in $games; do
    ls -la "build/web/$g.html" "build/web/$g.js" \
           "build/web/$g.wasm" "build/web/$g.data"
done

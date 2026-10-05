#!/bin/bash
#
# make-site.sh -- assemble a servable copy of the site from the tracked
# public/ shell plus the freshly built game packages in build/web/.
#
# public/ itself is never written to, so the shipped reference build stays
# intact for comparison.
#
#   ./test/make-site.sh            # copy whatever is in build/web/
#   ./test/make-site.sh doom1      # copy one game only
#
# then:  (cd build/test_site && python3 -m http.server 8123)

set -e
cd "$(dirname "$0")/.."

games="$*"
if [ -z "$games" ]; then
    # whatever build.sh last produced
    games=$(ls build/web/*.js 2>/dev/null | sed 's|.*/||; s|\.js$||' | tr '\n' ' ')
fi
if [ -z "$games" ]; then
    echo "make-site.sh: nothing in build/web/ -- run ./build.sh first" >&2
    exit 1
fi

for g in $games; do
    for ext in js wasm data; do
        if [ ! -f "build/web/$g.$ext" ]; then
            echo "make-site.sh: missing build/web/$g.$ext -- run ./build.sh $g" >&2
            exit 1
        fi
    done
done

site=build/test_site
mkdir -p "$site"
cp public/index.html public/index.css public/index.js "$site/"
cp -r public/fonts public/img "$site/"
cp public/preview.gif "$site/"

for g in $games; do
    cp "build/web/$g.js" "build/web/$g.wasm" "build/web/$g.data" "$site/"
done

echo "staged [$games] into $site/"
echo "  (cd $site && python3 -m http.server 8123)"

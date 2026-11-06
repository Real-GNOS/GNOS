#!/bin/sh
set -eu

usage() {
    echo "usage: $0 [count]" >&2
    echo "env: MAKE, CC, REPEAT_OUT" >&2
    exit 2
}

count=${1:-3}
case "$count" in
    ''|*[!0-9]*) usage ;;
esac

[ "$count" -ge 1 ] || usage
makecmd=${MAKE:-make}
cc=${CC:-}
out=${REPEAT_OUT:-build/repeat-boot}
mkdir -p "$out"

i=1
while [ "$i" -le "$count" ]; do
    log="$out/boot-$i.log"
    printf 'repeat_boot: %d/%d\n' "$i" "$count"
    if [ -n "$cc" ]; then
        if "$makecmd" test CC="$cc" >"$log" 2>&1; then
            :
        else
            rc=$?
            echo "repeat_boot: FAIL at iteration $i (rc=$rc); log: $log" >&2
            tail -40 "$log" >&2 || true
            [ -f build/dbg.log ] && tail -40 build/dbg.log >&2 || true
            exit "$rc"
        fi
    else
        if "$makecmd" test >"$log" 2>&1; then
            :
        else
            rc=$?
            echo "repeat_boot: FAIL at iteration $i (rc=$rc); log: $log" >&2
            tail -40 "$log" >&2 || true
            [ -f build/dbg.log ] && tail -40 build/dbg.log >&2 || true
            exit "$rc"
        fi
    fi
    cp build/dbg.log "$out/dbg-$i.log"
    i=$((i + 1))
done

echo "repeat_boot: $count/$count passed; logs in $out"

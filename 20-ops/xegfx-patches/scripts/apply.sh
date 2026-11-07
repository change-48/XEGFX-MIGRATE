#!/bin/bash
# Apply the accelerator patch series to the 5a19a73f tree, in order, then say what to do next.
# Nothing is installed: applying patches only changes the source tree.
#
# Usage: scripts/apply.sh [base|0001|...|0004|all]
#   base   restore state/nvrm-accel.cpp.base over the tree (undo everything)
#   all    apply 0001..0004 in order
set -u

P=$(cd "$(dirname "$0")/.." && pwd)
A1=$HOME/src/NvGfx-5a19a73f36249efb99c44c3f9cb34603ef0c0eb8
F=$A1/kexts/NVRM/accel/nvrm-accel.cpp
what=${1:-all}

restore() { cp -f "$P/state/nvrm-accel.cpp.base" "$F"; echo "tree restored to the 4f445170 baseline"; }

apply_one() {
    local f=$1
    [ -f "$P/patches/$f" ] || { echo "missing patch $f"; return 1; }
    if patch -p1 -d "$A1" --forward --batch < "$P/patches/$f" >/tmp/apply-$f.log 2>&1; then
        echo "applied $f"
    else
        echo "FAILED  $f  (see /tmp/apply-$f.log)"; return 1
    fi
}

case "$what" in
    base) restore ;;
    0001|0002|0003|0004)
        f=$(ls "$P/patches/$what"*.patch 2>/dev/null | head -1)
        [ -n "$f" ] || { echo "no such patch: $what"; exit 1; }
        apply_one "$(basename "$f")" ;;
    all)
        restore
        for f in 0001 0002 0003 0004; do
            apply_one "$(basename "$(ls "$P/patches/$f"*.patch | head -1)")" || exit 1
        done
        echo
        echo "now run: scripts/verify.sh accel   (expect OUT OF SYNC -- installing is a separate, recorded step)"
        ;;
    *) echo "usage: $0 [base|0001|0002|0003|0004|all]"; exit 1 ;;
esac

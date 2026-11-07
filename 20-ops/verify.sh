#!/bin/bash
# Rebuild every kext from its tree and compare it with what is installed.
#
# IN SYNC     -> the tree describes the machine; a change can be attributed.
# OUT OF SYNC -> the source you are looking at is NOT what the machine runs.  Settle that first.
#
# Usage: scripts/verify.sh [all|accel|fb]
set -u

A1=$HOME/src/NvGfx-5a19a73f36249efb99c44c3f9cb34603ef0c0eb8      # accelerator + core
A2=$HOME/src/NvGfx-xe-2.6.2                                       # panel kext + plugin
OUTK=$HOME/src/NvGfx-build/kexts

# The accelerator script takes OGKM only.  The panel kext needs an SDK, and its own default
# (MacOSX15.5.sdk) does not exist on this machine.  Never export SDKROOT globally: the
# accelerator build breaks when it inherits one.
export OGKM=${OGKM:-$HOME/src/deps/open-gpu-615.78.08}

pick_sdk() {
    local s
    for s in MacOSX15.5.sdk MacOSX15.4.sdk MacOSX15.sdk MacOSX.sdk; do
        if [ -d "/Library/Developer/CommandLineTools/SDKs/$s" ]; then
            echo "/Library/Developer/CommandLineTools/SDKs/$s"
            return 0
        fi
    done
    return 1
}
FB_SDK=$(pick_sdk)

rc=0
sha12() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1 | cut -c1-12; }

# $1 name  $2 tree  $3 script  $4 sdk (may be empty)  $5 built binary  $6 installed binary
check() {
    local name=$1 tree=$2 script=$3 sdk=$4 out=$5 inst=$6
    printf "%-14s building ... " "$name"
    if ! ( cd "$tree" && env -u SDKROOT SDKROOT="$sdk" ./build/nvgfx/$script >"/tmp/verify-$name.log" 2>&1 ); then
        printf "BUILD FAILED  (see /tmp/verify-%s.log)\n" "$name"
        rc=1
        return
    fi
    local a b
    a=$(sha12 "$out")
    b=$(sha12 "$inst")
    if [ -n "$a" ] && [ "$a" = "$b" ]; then
        printf "IN SYNC      %s\n" "$a"
    else
        printf "OUT OF SYNC  tree=%s  installed=%s\n" "${a:-none}" "${b:-none}"
        rc=1
    fi
}

want=${1:-all}

if [ "$want" = all ] || [ "$want" = accel ]; then
    check XeGfxAccel "$A1" "build_nvaccel.sh xegfxaccel" "" \
        "$OUTK/XeGfxAccel/26/XeGfxAccel.kext/Contents/MacOS/XeGfxAccel" \
        "/Library/Extensions/XeGfxAccel.kext/Contents/MacOS/XeGfxAccel"
fi

if [ "$want" = all ] || [ "$want" = fb ]; then
    check XeGfxIntelFB "$A2" "build_xegfxintelfb.sh" "$FB_SDK" \
        "$OUTK/XeGfxIntelFB/XeGfxIntelFB.kext/Contents/MacOS/XeGfxIntelFB" \
        "/Library/Extensions/XeGfxIntelFB.kext/Contents/MacOS/XeGfxIntelFB"
fi

echo
if [ $rc = 0 ]; then
    echo "trees describe the machine."
else
    echo "at least one tree no longer describes the machine -- settle that before changing anything."
fi
exit $rc

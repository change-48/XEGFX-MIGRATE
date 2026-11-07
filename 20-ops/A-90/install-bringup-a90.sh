#!/bin/bash
# A-90: install the patched bring-up script (display-wake before WindowServer restart).
# Backs up the running copy + its sha256 under ~/xegfx-log/A-90/backup/ first.
set -u
SRC=${1:?usage: install-bringup-a90.sh <new script>}
L=$HOME/xegfx-log/A-90/backup
mkdir -p "$L"
TGT=/usr/local/bin/xegfx-bringup.sh

cp -p "$TGT" "$L/xegfx-bringup.sh.before"
shasum -a 256 "$TGT" | tee "$L/sha.before"

bash -n "$SRC" || { echo "SYNTAX FAIL - not installing"; exit 1; }
sudo cp -f "$SRC" "$TGT"
sudo chown root:wheel "$TGT"
sudo chmod 755 "$TGT"
echo "--- installed ---"
shasum -a 256 "$TGT" | tee "$L/sha.after"
bash -n "$TGT" && echo "installed script parses OK"
grep -c "A-90" "$TGT"

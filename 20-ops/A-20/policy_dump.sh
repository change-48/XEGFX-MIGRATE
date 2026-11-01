#!/bin/bash
set -u
DB=/var/db/SystemPolicyConfiguration/KextPolicy
echo "== tables =="
sqlite3 "$DB" ".tables"
echo "== history schema =="
sqlite3 "$DB" ".schema kext_load_history_v3"
echo "== policy rows =="
sqlite3 "$DB" "select team_id,bundle_id,allowed,developer_name,flags from kext_policy;"
echo "== history rows (ours) =="
sqlite3 "$DB" "select bundle_id,path,cdhash from kext_load_history_v3 where bundle_id in ('org.watchthelight.XeGfx','org.watchthelight.XeGfxAccel','com.nullmoth.NMIntelFB','org.xenative.XeNative','org.watchthelight.IGPUConsoleRange');"

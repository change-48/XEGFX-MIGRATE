#!/bin/bash
set -u
DB=/var/db/SystemPolicyConfiguration/KextPolicy
IDS="org.watchthelight.XeGfx org.watchthelight.XeGfxAccel com.nullmoth.NMIntelFB org.xenative.XeNative"
sudo cp -p "$DB" "$DB.bak-A20-$(date +%H%M%S)"
for id in $IDS; do
  sudo sqlite3 "$DB" "delete from kext_load_history_v3 where bundle_id='$id';"
  sudo sqlite3 "$DB" "delete from kext_policy where bundle_id='$id';"
done
sudo sqlite3 "$DB" "insert into kext_policy (team_id,bundle_id,allowed,developer_name,flags) values ('','org.watchthelight.XeGfx',1,'Unidentified - XeGfx',0);"
sudo sqlite3 "$DB" "insert into kext_policy (team_id,bundle_id,allowed,developer_name,flags) values ('','org.watchthelight.XeGfxAccel',1,'Unidentified - XeGfxAccel',0);"
sudo sqlite3 "$DB" "insert into kext_policy (team_id,bundle_id,allowed,developer_name,flags) values ('','com.nullmoth.NMIntelFB',1,'Unidentified - NMIntelFB',0);"
sudo sqlite3 "$DB" "insert into kext_policy (team_id,bundle_id,allowed,developer_name,flags) values ('','org.xenative.XeNative',1,'Unidentified - XeNative',0);"
echo "== after =="
sudo sqlite3 "$DB" "select bundle_id,allowed,flags from kext_policy;"
sudo sqlite3 "$DB" "select count(*) from kext_load_history_v3;"

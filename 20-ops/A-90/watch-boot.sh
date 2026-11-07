#!/bin/bash
# wait for the Mac to come back from a reboot and for bringup to finish, then dump the boot's log tail.
# usage: watch-boot.sh <label>
ssh -i ~/.ssh/id_xegfx_remote -o ConnectTimeout=5 -o BatchMode=yes change48@IP-1 true 2>/dev/null \
  && echo "(still up -- waiting for it to go down)"
for i in $(seq 1 40); do
  sleep 5
  ssh -i ~/.ssh/id_xegfx_remote -o ConnectTimeout=5 -o BatchMode=yes change48@IP-1 true 2>/dev/null || continue
  echo "ssh up after ${i}x5s"
  break
done
# wait for bringup to reach a verdict, max 5 min
for i in $(seq 1 60); do
  OUT=$(ssh -i ~/.ssh/id_xegfx_remote -o ConnectTimeout=5 -o BatchMode=yes change48@IP-1 \
        "tail -25 /var/log/xegfx-bringup.log" 2>/dev/null)
  if echo "$OUT" | tail -6 | grep -qE "OK: frames are reaching the panel|WARN: iop_ok|abort"; then
    echo "$OUT"; exit 0
  fi
  sleep 5
done
echo "TIMEOUT waiting for bringup"
ssh -i ~/.ssh/id_xegfx_remote change48@IP-1 "tail -25 /var/log/xegfx-bringup.log" 2>/dev/null

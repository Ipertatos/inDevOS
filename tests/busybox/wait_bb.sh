#!/usr/bin/env bash
for i in $(seq 1 40); do
  if grep -q BUILD_EXIT= /mnt/c/osproj/bb_nohup.out 2>/dev/null; then
    echo FOUND
    break
  fi
  echo "tick $i makes=$(pgrep -c make || echo 0) $(date +%H:%M:%S)"
  sleep 20
done
echo "==== nohup ===="
cat /mnt/c/osproj/bb_nohup.out
echo "==== log tail ===="
tail -40 /mnt/c/osproj/bb_build.log
ls -la /home/ipertatos/src/busybox-1.36.1/busybox /mnt/c/osproj/tests/busybox/build/busybox.exe 2>&1

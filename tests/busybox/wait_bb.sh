#!/usr/bin/env bash
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/../.." && pwd)}"; export OSPROJ  # repo root
for i in $(seq 1 40); do
  if grep -q BUILD_EXIT= ${OSPROJ}/bb_nohup.out 2>/dev/null; then
    echo FOUND
    break
  fi
  echo "tick $i makes=$(pgrep -c make || echo 0) $(date +%H:%M:%S)"
  sleep 20
done
echo "==== nohup ===="
cat ${OSPROJ}/bb_nohup.out
echo "==== log tail ===="
tail -40 ${OSPROJ}/bb_build.log
ls -la ${HOME}/src/busybox-1.36.1/busybox ${OSPROJ}/tests/busybox/build/busybox.exe 2>&1

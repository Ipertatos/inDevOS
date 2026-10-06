#!/usr/bin/env bash
# Post-boot serial log sanity checks (run against serial.log or QEMU -serial file).
# Usage: validate_boot_expect.sh path/to/serial.log
set -euo pipefail
log="${1:-serial.log}"
if [[ ! -f "$log" ]]; then
  echo "Missing log: $log"
  exit 1
fi
fail=0
grep -q "Driver adapters: Registered" "$log" || { echo "FAIL: driver adapters registration line"; fail=1; }
grep -q "Driver manager: Auto-discovery complete" "$log" || { echo "FAIL: auto-discovery line"; fail=1; }
grep -q "net: using" "$log" || grep -q "no supported PCI ethernet" "$log" || { echo "FAIL: expected net selection line"; fail=1; }
grep -q "CPU protections: NX=" "$log" || { echo "FAIL: CPU protections line"; fail=1; }
grep -q "Zeus Shell" "$log" || { echo "FAIL: Zeus shell did not start"; fail=1; }
grep -q "System ready" "$log" || { echo "FAIL: System ready line"; fail=1; }
grep -q "per-CPU run queues" "$log" || grep -q "Single CPU system" "$log" || {
  echo "FAIL: SMP status line"; fail=1;
}
if grep -q "SMAP=on" "$log"; then
  grep -q "SMAP self-test: enforced" "$log" || { echo "FAIL: SMAP was on but self-test missing"; fail=1; }
fi
if grep -q "PAGE FAULT" "$log" && ! grep -q "demand-page" "$log"; then
  echo "WARN: page-fault diagnostics present"
fi
grep -q "\[drm\]" "$log" || { echo "WARN: drm init not found (optional)"; }
exit "$fail"

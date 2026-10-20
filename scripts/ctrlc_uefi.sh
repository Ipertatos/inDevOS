#!/usr/bin/env bash
# Old forksig suite + KILLPGID (suite.sh) + QMP Ctrl-C tests (interactive ash).
WINHOME="${WINHOME:-$(wslpath "$(cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r')" 2>/dev/null || echo "$HOME")}"; export WINHOME  # Windows user profile (logs, ws_* scratch)
set -u
T=/tmp/indev_uefi_suite
LOG=${WINHOME}/ws_uefi/uefi_ctrlc.log
exec > >(tee "$LOG") 2>&1
echo "==== START $(date) ===="
cd ${WINHOME}/ws_uefi
SRCD=${WINHOME}/forksig_tests_src
: # private copy: reuse already-built forksig exes
for f in suite.sh loop.sh; do :; done
mcopy -o -i sata_disk.img ${WINHOME}/suite.sh ::/home/user/forksig/suite.sh
mcopy -o -i sata_disk.img ${WINHOME}/ctrlc_loop.sh ::/home/user/forksig/loop.sh
for f in sigint_count kill_pgid; do mcopy -o -i sata_disk.img "$SRCD/build/$f.exe" ::/home/user/forksig/$f.exe; done
: # private copy: never kill other streams QEMU
rm -rf "$T"; mkdir -p "$T"
cp -f os.iso "$T/boot.iso"; cp -f sata_disk.img "$T/sata_disk.img"
qemu-system-x86_64 -boot d \
  -drive "file=$T/boot.iso,format=raw,index=2,media=cdrom" \
  -smp 4 -m 1G -M pc \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -drive "file=$T/sata_disk.img,format=raw,index=0" \
  -device virtio-vga,xres=1920,yres=1080 -display none -no-reboot \
  -serial "file:$T/serial.log" -qmp "tcp:127.0.0.1:4550,server,nowait" \
  >"$T/qemu.log" 2>&1 &
QPID=$!
echo QPID=$QPID
for i in $(seq 1 150); do grep -aq "mouse\] POLL: first" "$T/serial.log" 2>/dev/null && break; sleep 1; done
echo "boot-ready after ${i}s"; sleep 8
for attempt in 1 2 3 4; do
  python3 qmp_step.py 4550 "$T/m$attempt" "wait:2" "type:mount 0" "wait:20" || true
  grep -aq "Mounting FAT32 filesystem from device 0" "$T/serial.log" && { echo "mount OK $attempt"; break; }
  echo "mount retry $attempt"; sleep 3
done
echo "==== CTRL-C PHASE (fresh boot, before suite) ===="
python3 qmp_step.py 4550 "$T/c" \
  "type:/bin/busybox sh" "wait:6" \
  "type:echo ASH_UP" "wait:3" \
  "type:busybox sleep 100" "wait:4" "key:ctrl,c" "wait:3" "type:echo CTRLC_A_OK" "wait:3" \
  "type:busybox sh /home/user/forksig/loop.sh | busybox cat" "wait:5" "key:ctrl,c" "wait:4" "type:echo CTRLC_B_OK" "wait:3" \
  "type:/home/user/forksig/sigint_count.exe" "wait:4" "key:ctrl,c" "wait:4" "type:echo CTRLC_C_DONE" "wait:3" \
  "type:exit" "wait:5" \
  "type:echo ZEUS_NATIVE_OK" "wait:3" "type:ls /home/user/forksig" "wait:4" || true
echo "==== OLD SUITE (typed into Zeus after ash exit) ===="
for attempt in 1 2 3; do
  python3 qmp_step.py 4550 "$T/s$attempt" "wait:2" "type:/bin/busybox sh /home/user/forksig/t" "wait:15" || true
  grep -aq "SUITE_BEGIN" "$T/serial.log" && { echo "suite started $attempt"; break; }
  echo "suite retry $attempt"; sleep 3
done
for i in $(seq 1 420); do grep -aq "SUITE_END" "$T/serial.log" && break; sleep 1; done
echo "SUITE_END after ${i}s"; sleep 4
sleep 2
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
cp -f "$T/serial.log" ${WINHOME}/ws_uefi/uefi_serial_ctrlc.log
grep -aE "ASH_UP|ZEUS_NATIVE|kill_pgid.exe|zeus:/|SUITE_END|PASS |FAIL |tty: |kbd: Ctrl|CTRLC_|SIGC_|KILLPGID|kill_pgid|status=1[34][0-9]|Page fault|exception" "$T/serial.log" | grep -v kfree | tail -n 120
python3 - << 'PY'
import re, os
s=open("/tmp/indev_uefi_suite/serial.log","rb").read().decode("latin1","replace")
lines=s.splitlines()
def ts(l):
    m=re.match(r"\[\s*([0-9.]+)\]",l); return float(m.group(1)) if m else None
old_keys=["PIPEOK","NESTFORK","SIGCHLDFLAG","YESHEAD","SIGSELF","FORKPRINT","FORKCOW","TCC","KILLPGID"]
old={k:("PASS "+k in s and "FAIL "+k not in s) for k in old_keys}
sigp=sum(1 for i in range(1,6) if f"PASS SIGPIPE_{i}" in s)
fst=sum(1 for i in range(1,6) if f"PASS FORKSTACK_{i}" in s)
pf=s.count("Page fault")
sig_idx=[i for i,l in enumerate(lines) if "tty: signal 2 ->" in l]
def marker(name):
    return any(re.search(r"\]\s*"+name+r"\s*$", l) for l in lines)
def exits_after(i, until_marker, window=None):
    t0=ts(lines[i]); n=0; first=None
    for l in lines[i+1:]:
        if until_marker in l: break
        if "status=130 -> zombie" in l or "status=130" in l and "exec_spawned_exit" in l:
            t=ts(l)
            if window is None or (t is not None and t0 is not None and t-t0<=window):
                n+=1; first=first or t
    return n, (first-t0 if first and t0 else None)
A=B=C=False; detail={}
if len(sig_idx)>=1:
    n,dt=exits_after(sig_idx[0],"CTRLC_A_OK",2.5); A = n>=1 and marker("CTRLC_A_OK"); detail["A"]=(n,dt)
if len(sig_idx)>=2:
    n,dt=exits_after(sig_idx[1],"CTRLC_B_OK"); B = n>=2 and marker("CTRLC_B_OK"); detail["B"]=(n,dt)
C = "SIGC_SURVIVED hits=1" in s and marker("CTRLC_C_DONE")
zn = any(re.search(r"\]\s*ZEUS_NATIVE_OK\s*$", l) for l in lines)
line=(f"ZEUSNATIVE={int(zn)} CTRLC A={int(A)} B={int(B)} C={int(C)} D={int(old['KILLPGID'])} sigs={len(sig_idx)} {detail} | "
      f"OLD {old} SIGPIPE={sigp}/5 FORKSTACK={fst}/5 PF={pf}")
print(line)
open(os.environ["WINHOME"]+"/ws_uefi/uefi_score.txt","w").write(line+"\n")
if all(old.values()) and sigp>=4 and fst==5 and A and B and C:
    open(os.environ["WINHOME"]+"/ws_uefi/uefi_score.txt","a").write("MILESTONE_GREEN\n")
PY
if grep -q MILESTONE_GREEN ${WINHOME}/ws_uefi/uefi_score.txt && [ "${BUILD_ISO:-0}" = 1 ]; then
  echo "==== INSTALL ISO ===="
  bash make_install_iso.sh 2>&1 | tee ${WINHOME}/forksig_install_iso.log | tail -5
  ls -la os_install.iso | tee -a ${WINHOME}/ws_uefi/uefi_score.txt
fi
echo CTRLC_SUITE_DONE | tee -a ${WINHOME}/ws_uefi/uefi_score.txt
echo "==== END $(date) ===="

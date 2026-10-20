#!/usr/bin/env bash
set -uo pipefail
LOG=/mnt/c/Users/froze/forksig_tests_nohup.log
exec > >(tee "$LOG") 2>&1
echo "==== START $(date) ===="
cd /mnt/c/osproj

echo "==== BUILD ===="
make -j"$(nproc)" kernel_final.bin os.iso 2>&1 | tee /mnt/c/Users/froze/forksig_build_suite.log | tail -12
echo BUILD:$?

mcopy -o -i sata_disk.img /mnt/c/Users/froze/suite.sh ::/home/user/forksig/suite.sh
mcopy -o -i sata_disk.img /mnt/c/Users/froze/pipeok.sh ::/home/user/forksig/pipeok.sh
mcopy -o -i sata_disk.img /mnt/c/Users/froze/yeshead.sh ::/home/user/forksig/yeshead.sh
for f in fork_stack sig_self fork_print fork_cow pipe_sigpipe nest_fork sigchld_flag; do
  src=""; [ -f tests/forksig/build/${f}.exe ] && src=tests/forksig/build/${f}.exe; [ -f /mnt/c/Users/froze/forksig_tests_src/build/${f}.exe ] && src=/mnt/c/Users/froze/forksig_tests_src/build/${f}.exe; [ -n "$src" ] && mcopy -o -i sata_disk.img "$src" ::/home/user/forksig/${f}.exe || true
done

T=/tmp/indev_suite
rm -rf "$T"; mkdir -p "$T"
cp -f os.iso "$T/boot.iso"
cp -f sata_disk.img "$T/sata_disk.img"

qemu-system-x86_64 -boot d \
  -drive "file=$T/boot.iso,format=raw,index=2,media=cdrom" \
  -smp 4 -m 1G -M pc \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -drive "file=$T/sata_disk.img,format=raw,index=0" \
  -device virtio-vga,xres=1920,yres=1080 -display none -no-reboot \
  -serial "file:$T/serial.log" -qmp "tcp:127.0.0.1:4498,server,nowait" \
  >"$T/qemu.log" 2>&1 &
QPID=$!
echo QPID=$QPID
sleep 70

python3 qmp_scenario.py 4498 "$T/shot" \
  "wait:8" "type:mount 0" "wait:30" \
  "wait:8" "type:busybox sh /home/user/forksig/suite.sh" "wait:420" \
  || true

kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
cp -f "$T/serial.log" /mnt/c/Users/froze/serial-suite.log

echo "==== HIGHLIGHTS ===="
grep -aE "SUITE_|pipe_sigpipe n=|fork_stack OK|YESHEAD|hit=|waited|parent g=|hello from|Page fault|write: EPIPE|SUITE_END" "$T/serial.log" | grep -v kfree | tail -n 100 || true

python3 - << 'PY'
import re
s=open("/tmp/indev_suite/serial.log","rb").read().decode("latin1","replace")
ended="SUITE_END" in s
sigs=re.findall(r"pipe_sigpipe n=(-?\d+) errno=(\d+) got=(\d+)", s)
sig_ok=sum(1 for n,e,g in sigs if (n=="-1" and e=="32") or g=="1")
fs_ok=s.count("fork_stack OK")
# PIPEOK: bare 6 between PIPEOK_BEGIN and PIPEOK_END, or PIPEOK string
m=re.search(r"SUITE_PIPEOK_BEGIN(.*?)SUITE_PIPEOK_END", s, re.S)
pipe=False
if m:
    chunk=m.group(1)
    pipe=bool(re.search(r"(?m)(?:^|\])\s*6\s*$", chunk) or "PASS PIPEOK" in chunk or re.search(r"(?m)^6$", chunk))
yes="YESHEAD_OK" in s
core=all(x in s for x in ("hit=1","waited","parent g=","hello from"))
sigchld=("sigchld_flag flag=1" in s or "PASS SIGCHLDFLAG" in s)
pf=s.count("Page fault")
print(f"ended={ended} SIGPIPE={sig_ok}/5({sigs}) FORKSTACK={fs_ok}/5 PIPE={int(pipe)} CORE={int(core)} YES={int(yes)} SIGCHLD={int(sigchld)} PF={pf}")
open("/mnt/c/Users/froze/forksig_score.txt","w").write(
  f"SUITE SIGPIPE={sig_ok}/5 FORKSTACK={fs_ok}/5 PIPE={int(pipe)} CORE={int(core)} YES={int(yes)} SIGCHLD={int(sigchld)} PF={pf} END={int(ended)}\n")
if pipe and core and sig_ok>=4:
    open("/mnt/c/Users/froze/forksig_score.txt","a").write("MILESTONE_GREEN\n")
PY

if grep -q MILESTONE_GREEN /mnt/c/Users/froze/forksig_score.txt; then
  echo "==== INSTALL ISO ===="
  bash make_install_iso.sh 2>&1 | tee /mnt/c/Users/froze/forksig_install_iso.log | tail -15
  ls -la os_install.iso | tee -a /mnt/c/Users/froze/forksig_score.txt
fi
echo SUITE_DONE | tee -a /mnt/c/Users/froze/forksig_score.txt
echo "==== END $(date) ===="

#!/usr/bin/env bash
M=/home/ipertatos/indev_tcc_ws/stage/home/user/mk
cat > $M/self.sh <<'S'
cd /usr/src/tcc
echo SELF_BEGIN
tcc -DONE_SOURCE=1 -DTCC_TARGET_X86_64 -o /tmp/tcc2 tcc.c 2>&1
echo RC1=$?
busybox ls -l /tmp/tcc2 2>&1
/tmp/tcc2 -v 2>&1
/tmp/tcc2 -o /tmp/h2 /home/user/h2.c 2>&1
echo RC2=$?
/tmp/h2
echo RC3=$?
echo SELF_END
S
cat > $M/mk.sh <<'S'
cd /home/user/mk
echo MK_BEGIN
make 2>&1
echo RCMK=$?
echo MK_END
S
ls -l $M

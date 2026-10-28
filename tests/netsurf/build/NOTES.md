# NetSurf 3.11 -> inDevOS (build notes, 2026-10-24)
- Build: ./build_netsurf.sh  -> tests/netsurf/netsurf (static musl ELF EXEC, no interp, 9,397,632 B stripped; 19,266,360 unstripped), tests/netsurf/res (1.1 MB with fonts/, which is dropped)
- Features: curl 8.10.1 + OpenSSL 3.0.15 + zlib, PNG, JPEG, GIF, BMP, SVG, RISC OS sprite, PSL, nslog, Duktape JS, utf8proc; freetype 2.13.3 + DejaVu TTFs (Greek etc.). Off: WebP, JPEG-XL, video, PDF export.
- libnsfb surfaces: ram (stock) + wm (patches/nsfb_wm.c, /dev/wm ABI v1.1; lowest enum => default). No linux-fbdev surface in libnsfb 0.2.x; SDL/X/VNC/Wayland not built.
- Resource search path (NETSURF_FB_RESPATH): ~/.netsurf/, $NETSURFRES, /etc/netsurf/, /cdrom/etc/netsurf/, /usr/share/netsurf/, /cdrom/usr/share/netsurf/.
- Host smoke test (ram surface): HOME=/tmp/nst NETSURFRES=$PWD/tests/netsurf/res/ tests/netsurf/netsurf -v -f ram file:///tmp/nst/t.html
- Smoke (ram, 800x600): file:///tmp/nst/t.html -> DOM->box complete, redraw, history "inDevOS smoke". https://example.com -> "Example Domain" (needs ~/.netsurf/Choices ca_bundle:<pem>). Peak RSS ~16 MB.
- Syscalls seen: writev mmap readlink munmap rt_sigaction read stat access brk open fcntl close uname set_tid_address pipe lseek getuid/gid/euid/egid execve arch_prctl getpid rt_sigprocmask socket poll sendto recvmsg getsockname write setsockopt getsockopt mprotect getrandom futex clone(CLONE_THREAD) connect bind exit — all present in inDevOS linux_syscall.c @d176bd1.
- Known open items: BusyBox `[ -x /bin/netsurf ]` is false (kernel reports no exec mode bits); desktop icons have no Enter-to-launch; `dhcp -a` blocks ~10 s when no DHCP server answers.

# QuickJS (quickjs-ng) on inDevOS

## Version: quickjs-ng v0.17.0 (2026-09-18)
Chosen over Bellard's QuickJS (latest 2026-06-04) because quickjs-ng is actively maintained
(frequent releases, CI on many platforms), tracks newer ES features (ES2023+: toSorted/findLast,
Object.groupBy, class static blocks, Iterator helpers...), exposes a stable embedding API that is a
superset of Bellard's (JS_NewRuntime/JS_Eval/JS_NewClass...), and builds from plain .c files
(no configure step), which suits a musl-static cross build. Source pinned by sha256 in build_quickjs.sh.

## Build (host/box, Debian + musl-tools)
    TEST262=1 tests/quickjs/build_quickjs.sh /path/to/workdir      # ~30 s
Produces WORKDIR/dist/{lib/libquickjs.a, lib/libquickjs-libc.a, include/quickjs*.h, bin/qjs, qjsc,
run-test262, api-test}. All static EXEC, no PT_INTERP, no dynamic linking.
Flags: -O2 -g -std=gnu11 -D_GNU_SOURCE -DQUICKJS_NG_BUILD -fno-strict-aliasing; link -static -lm -pthread.
Embedders: `musl-gcc -static ... -Iinclude lib/libquickjs.a -lm -pthread`.
(NetSurf's backend currently links Bellard's QuickJS 2025-04-26, built by tests/netsurf/build/build_netsurf.sh.)
Local source patches go in tests/quickjs/patches/*.patch (applied -p1 in order; currently none needed).

## Tests
* `smoke.js` (+ `smoke_worker.js`): 24 OS-facing checks (FPU/libm/dtoa/BigInt/ES2023/regexp/JSON/Date,
  clock, os.sleep, fs, readdir, env, cwd, os.exec wait+pipe, signals, timers, promises, Worker+Atomics).
  `qjs --std smoke.js [only=a,b] [skip=c]` -> prints `SMOKE x/y`.
* `kstress/mmthreads.c`: kernel stress for concurrent mmap/malloc from pthreads (`mmthreads [threads] [n] [fork]`).
* upstream `api-test`, `run-test262 -c tests.conf` (qjs's own tests), `run-test262 -c test262.conf`
  on a 37-directory test262 subset (15335 tests).

## Results (box QEMU TCG, 4 vCPU, 2 GB)
| suite | host (box, native) | inDevOS kfix2 (954824a+net fixes) | inDevOS + 0001 + 0002 |
|---|---|---|---|
| smoke.js (24) | 24/24 | 21/21 without threads, Worker crashes when combined | 24/24 x4 (one 23/24 timer-order flake on 0001 only) |
| mmthreads 2/4/8 thr | OK | crash/hang with >=2 threads | bad=0 all, incl. after fork |
| api-test | pass | SIGSEGV (stack, see #2) | pass |
| tests.conf (qjs own, 116) | 0/116 errors | n/a | 0/116 errors (needs writable cwd) |
| test262 subset 15335, -t1 | 7/15335 errors, 84 s | n/a | 7/15335 errors (identical), ~20 min under TCG |
| full test262 (82220) | 49 errors = known list | not run (TCG too slow) | not run |

test262 subset dirs: vm/subset.txt. inDevOS pass rate on the subset = host pass rate (99.95%).

## Kernel findings (both fixes are merged in f99c280)
1. **0001-mm-shared-thread-address-space.patch** — CLONE_VM threads are separate process_t sharing the
   pml4, but fork_spawn_child copies brk_current/mmap_anon_next by value and mmap_regions starts NULL.
   Two threads calling mmap get the same VA (PTEs overwritten; one thread's munmap frees the other's
   pages; vmm_get_next_level races). Symptoms: Worker #PF in __JS_FindAtom, musl mallocng assert,
   mmthreads 2+ threads crash/hang. Fix: process_t.mm_leader; brk/mmap/munmap/mprotect/mmap_device
   operate on the leader under one spinlock. Verified: mmthreads 2/4/8 bad=0, smoke 24/24.
2. **0002-user-stack-8mib.patch** — the demand-paging stack window in idt.c is hard-coded to 1 MiB
   (exec premaps 256 pages). QuickJS's default JS stack limit is 1 MiB measured from the runtime's
   creation point, so deep recursion overruns the real stack before JS throws RangeError → SIGSEGV
   (api-test async_call_stack_overflow, #PF at 0x7fffffefea68). Fix: USER_STACK_LIMIT = 8 MiB
   (Linux default RLIMIT_STACK) for both stack windows; exec still premaps only 1 MiB. api-test passes.
   Without the patch, embedders must call `JS_SetMaxStackSize(rt, 512*1024)` (recommended for NetSurf anyway).
3. Not fixed / open:
   * idt.c heap demand-paging check uses the faulting thread's (stale) brk copy — should use mm_leader.
   * A thread killed by a fault never does the CLONE_CHILD_CLEARTID futex wake → pthread_join hangs;
     busybox `timeout` cannot kill that process.
   * After run-test262 (which forks via os.exec tests) a child is left alive and reparented
     ("reparented 1 to 3" in serial) holding the stdout pipe, so `run-test262 | tail` never sees EOF.
     Workaround: redirect to a file. On inDevOS run-test262 -c tests.conf exits 1 with 0 errors (host: 0).
   * qjs-libc timer-order flake (expired timers fire in insertion order after a stalled loop) is a
     qjs quirk under slow TCG, not a kernel bug.
No problems found with FPU/SSE, libm, clock_gettime, nanosleep (sleep(200)=201 ms),
Atomics.wait timeouts, signals, fs, or exec.

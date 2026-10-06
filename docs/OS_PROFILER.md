# OS Profiler (inDevOS)

The kernel profiler records **combined** data for performance work:

| Kind | Meaning |
|------|---------|
| 1 (`SAMPLE`) | LAPIC ISR sample: RIP, RBP, 2 stack return addresses (`unused` = user-mode flag) |
| 2 (`SC_ENTER`) | Syscall entered (`arg0` = syscall number) |
| 3 (`SC_EXIT`) | Syscall exited (`arg0` = nr, `arg1` = duration µs, `arg2` = return value) |
| 4 (`SCHED`) | Preemptive context switch: `pid` = from PID, record `tid` = from thread id, `arg0` = to thread id, `arg1` = to PID |
| 5 (`FRAME`) | `SDL_RenderPresent` / frame flip (`arg0` = window pointer token) |

## Commands (Zeus shell)

```
prof start [divisor]   # enable recording; divisor = LAPIC ISR sample stride (default 8)
prof stop              # disable
prof reset             # clear ring indices
prof stats             # counters and per-CPU ring depth
prof dump              # print KP,v1,... lines on serial — capture with serial.log
prof overlay on|off    # thin SDL strip: syscall/sample/sched/frame ratios
prof sample N          # set divisor only (N >= 1)
```

## Architecture

- **Per-CPU lock-free rings** (`src/profiler.c`): timer IRQ and syscall handlers both push without a global spinlock (avoids IRQ ↔ syscall deadlock).
- **Hooks**: LAPIC timer path in `src/idt.c`, `syscall_handler` wrapper in `src/syscall.c`, scheduler switch before register restore in `src/idt.c`, `SDL_RenderPresent` in `src/sdl_renderer.c`.

## Export to Chrome / Perfetto

1. Capture serial output while running workload.
2. Filter and convert:

```bash
grep '^KP,v1,' serial.log | python3 scripts/prof_to_chrome_trace.py > trace.json
```

Each dump line must be **`KP,v1,` plus ten payload fields**: `kind`, `unused`, `cpu`, `pid`, `tid`, `ts_us`, `arg0`–`arg3` (**12 comma-separated tokens** including `KP` and `v1`). Older kernels accidentally printed one extra trailing field; the converter truncates longer lines to twelve tokens.

3. Open `chrome://tracing` or [Perfetto UI](https://ui.perfetto.dev/) and load `trace.json`.

Syscall durations appear as **`X`** (complete) events; samples and other rows are **`i`** (instant) markers.

## Overhead / validation

| Factor | Mitigation |
|--------|-------------|
| Sample rate | Default **one sample per ~8 LAPIC ticks** (~4 ms effective at 500 µs/timer); tune with `prof start N`. |
| Ring overwrite | Fixed **2048 slots/CPU**, lossy when buffer wraps — use shorter runs or slower sampling. |
| Serial `dump` | High volume; capture only after `prof stop`; avoid dumping during tight loops. |
| SDL overlay | Resets aggregator each present; negligible unless thousands of SDL windows |

**Smoke check:** boot, `prof start 16`, exercise shell and an SDL app, `prof dump` and confirm non-empty `KP,v1,...` lines; compare interactive feel with profiler off (`prof stop`).

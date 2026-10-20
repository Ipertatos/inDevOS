# Third-party components

inDevOS itself is licensed under GPL-2.0-or-later (see `LICENSE`).
The following third-party code and data is included in, or used by, this repository.

| Component | Where | Licence | Notes |
|---|---|---|---|
| Limine bootloader | `limine/` (git submodule, v7.x-binary) | BSD-2-Clause | See `limine/LICENSE`. |
| Flanterm | `flanterm/` (git submodule) | BSD-2-Clause | See `flanterm/LICENSE`. |
| Lua 5.4.7 | `third_party/lua-5.4.7/` | MIT | See `third_party/lua-5.4.7/LICENSE`. Built into the kernel (LuaDE). |
| DOOM (linuxdoom-1.10) | `doom/` | GPL-2.0-or-later (id Software 1999 re-release) | See `doom/LICENSE-NOTE`. The WAD game data is **not** included. |
| TinyCC (tcc) runtime | `tests/tcc/build/libtcc1.a`, `tests/tcc/build/tcc_include/` | LGPL-2.1-or-later | Prebuilt TCC runtime library and headers. TCC itself is built separately by `tests/tcc/build_tcc.sh` from the upstream tinycc sources. |
| musl libc | not vendored | MIT | Used to build Linux-ABI userland (BusyBox, TCC, tests) from a separate musl install (`$HOME/.local/musl`). |
| BusyBox 1.36.1 | not vendored; `tests/busybox/busybox.config` | GPL-2.0 | Built separately by `tests/busybox/build_busybox.sh`. If you distribute an image that contains `busybox.exe`, the GPL requires you to offer the matching BusyBox source (upstream 1.36.1 + the config in this repo). |
| curl | not vendored (`tests/curl/` is git-ignored) | curl licence (MIT-style) | Static musl build used for testing; not part of this repo. |
| herb-engine-C-3D | `third_party/herb-engine-C-3D/` | **unknown** | From https://github.com/42henry/herb-engine-C-3D (see `scripts/fetch_herb_engine.sh`). No licence file upstream was found; **origin/licence needs confirming** before redistribution. |
| Paratrooper audio | `Paratrooper.wav`, `Paratrooper.raw`, `src/paratrooper.h` | **unknown** | Provenance not recorded. **Needs confirming** (or replacing) before redistribution. |

Reference material that is used locally but deliberately **not** committed: `ref/` (AMD GPU reference repos) and `doom1.wad`.
# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

The **stolmine fork** of the Orthogonal Devices ER-301 Sound Computer firmware: boot loaders (MLO/SBL), firmware (kernel.bin), a desktop emulator (Linux/macOS), factory mods (core, teletype, txo, multiout), and a mod SDK. Mixed C/C++ (DSP engine, HAL) and Lua (entire application/UI layer), bound with SWIG.

On top of upstream, this fork adds: the **dense unit picker**, **sequencers**, **scene mode** (tour: `GETTING_STARTED.md`), **crash diagnostics** (`docs/CRASH_REPORT_FORMAT.md`, `tools/symbolize_crash.py`), a **headless emulator** with an automated test harness (see Testing), and a **ledger-gated dev workflow** (see Dev workflow).

## Build Commands

The top-level Makefile delegates to `scripts/*.mk`. Two variables control every build:

- `PROFILE` = `testing` (default; logging + dev mode), `debug`, `release`
- `ARCH` = `linux` | `darwin` (auto-detected) | `am335x` (hardware cross-compile)

Outputs land in `$(PROFILE)/$(ARCH)/` (on Linux, `$(PROFILE)/linux-$(uname -m)/`).

```bash
make emu                      # Build the emulator
testing/darwin/emu/emu.elf    # Run it FROM REPO ROOT (finds xroot/)
make core && make core-install    # Build core mod, stage .pkg in ~/.od/rear
make teletype teletype-install txo txo-install multiout multiout-install
make firmware                 # am335x release zip (Linux + TI SDK; see docker/)
```

Do not combine a mod's build and install targets in one parallel invocation (`make core core-install` races two sub-makes over the same objects; run them sequentially or as separate targets).

`scripts/dev build` wraps make with a machine-wide build-slot semaphore and a job cap — prefer it when other agent sessions may be building in parallel worktrees.

## Versioning

`FIRMWARE_VERSION` comes from `git describe --match 'v*.*.*-*' --tags` (scripts/env.mk). Release tags are **annotated** and follow `v0.7.0-stolmine.<major>.<minor>.<patch>` (e.g. `v0.7.0-stolmine.9.6.0`, matching CHANGELOG.md headings); commits past the tag append a count segment. `scripts/dev push` publishes annotated tags via `--follow-tags`.

**A clone without tags builds "successfully" with an empty version** — broken zip/pkg names, blank UI version, misplaced state folder. If `git tag` is empty, fetch tags or cut an annotated tag on the latest release commit before building.

Because the version begins `0.7`, the emulator's user-state folder is `~/.od/front/ER-301/v0.7` — **shared with a stock-firmware emulator on the same machine** (as are `~/.od/rear/firmware.cfg`, `settings.lua`, and the `~/.od/rear` package drop zone).

## Testing

Unlike upstream, this fork has an automated test suite:

- `tests/emu/*.test` — headless-emulator UI/behavior tests run by `tools/emu_test.py` (TAP; hermetic per-test sandboxes; goldens in `testing-assets/emu/`). Format and conventions: `tests/emu/README.md`; design: `planning/headless-emu-plan.md`.
- `scripts/dev test` runs the suite gated (skips when no emu binary). Direct: `STOL_EMU_BIN=testing/darwin/emu/emu.elf python3 tools/emu_test.py [TEST...]` (default binary path is the Linux one). `STOL_EMU_PKG_DIR=testing/darwin/mods` supplies `.pkg` files to tests that declare `!packages`.
- The emulator's `--headless` mode speaks a lockstep stdin/stdout control protocol (`press`, `turn`, `frames`, `stable`, `cap`, `lua`, ...; replies sigiled with `@`) implemented via `emu/emu.cpp.swig`.
- **UI planning**: the UI is modeled as fluents + operators; `emu.uiState()` is the oracle and `tools/ui_solve.py` turns "reach state X" into executed, verified gestures. Read `docs/AGENT_EMU_GUIDE.md` before reverse-engineering any navigation from source — introspect and plan instead.
- Interactive smoke test: dev mode → admin → Test Console → `LoadAllUnits`.

## Dev workflow (ledger + gate)

`scripts/dev` is the front door: `build`, `test`, `check` (the gate), `commit`, `push`, `integrate`, plus ledger verbs (`claim`/`release`/`claims`, `ledger-append`, `render`, `status`, `now`). Work items live in a ledger (`planning/ledger.toml`, managed by `tools/ledger.py`); `planning/TODO.md` is **rendered from it** — never edit by hand. Commits go through `scripts/dev commit` (stamp + render + gate + fence); the fence rejects staged changes outside the branch's claimed footprint. Source comments carry `[stol:<item>]` tags linking code to ledger items. Never write a date from memory — use `scripts/dev now`.

## Architecture

Layers bottom-up: `hal/` (portable HAL contract as C headers) → per-target impls (`arch/am335x/hal/` TI-RTOS, `arch/linux/hal/`, `arch/darwin/hal/`, `emu/hal/` SDL) → `od/` C++ engine (AudioThread DSP graph; UIThread Lua + graphics; SWIG glue in `od/glue/`) → `xroot/` (the entire application in Lua; boot at `xroot/boot/`) → mods (`mods/*` → `.pkg` via `scripts/mod-builder.mk`). Firmware entry `app/app.cpp`; emulator entry `emu/`.

Fork-specific wiring worth knowing:
- `emu/emu.cpp.swig` exposes `emu.*` to Lua (headless control drain, `emu.uiState()`).
- Crash diagnostics span `hal/crash.h`, `hal/modulemap.h` (each arch implements `od::enumerateModules`: `arch/am335x/hal/dynload/dlfcn.cpp`, `arch/linux/hal/dynload.cpp`, `arch/darwin/hal/dynload.cpp`), and `od/glue/CrashDiag.cpp`.
- `docker/` cross-compiles am335x builds without a local TI SDK.

## Local build environment (this machine: Apple Silicon, macOS 26)

Verified 2026-08-12. Required environment for ALL builds (gcc@11's bottle targets a missing SDK; SDK 26 headers declare `_Float16` which g++-11 can't parse; Homebrew swig 4.5 dropped a flag this repo passes):

```bash
export SDKROOT=$(xcrun --show-sdk-path)
export MACOSX_DEPLOYMENT_TARGET=26.0
export CFLAGS="-D_Float16=__fp16"          # make seeds CFLAGS from env; makefiles += onto it
make emu GCC_VERSION=11 SWIG=$HOME/.local/bin/swig-er301
```

- `scripts/darwin.mk` selects **gcc-15** on arm64, which is not installed here; `GCC_VERSION=11` overrides it to the Homebrew gcc@11 that is (with its stale include-fixed headers already moved aside — see upstream's `~/Developer/er-301/CLAUDE.md` for the machine-wide fixes).
- `~/.local/bin/swig-er301` shim strips `-no-old-metatable-bindings` for swig 4.5.
- `arch/darwin/hal/dynload.cpp` maps `_Static_assert` → `static_assert` before mach headers because gcc (unlike clang) rejects the C keyword in C++; the macOS 26 SDK's `mach/message.h` depends on it.

## Agent bridge (emulator remote control)

The running (windowed) emulator can be driven programmatically: `er301ctl ping|state|screenshot|tap|toggle|eval ...` (CLI in `~/.local/bin`, backed by the `agentbridge` package; source at `~/Developer/er301-agentbridge`; `make install` there stages the .pkg for auto-install on next emulator boot). Screenshots are PNGs readable by Claude. Only one emulator may run per machine (the bridge channel `/tmp/er301-bridge` is a singleton, and `~/.od` is shared). See the er301-bridge skill. For scripted/deterministic interaction prefer the headless harness above.

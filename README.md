# ra8d2-firmware

[![firmware](https://github.com/bsikar/ra8d2-firmware/actions/workflows/firmware.yml/badge.svg?branch=main)](https://github.com/bsikar/ra8d2-firmware/actions/workflows/firmware.yml)

Bare-metal firmware for the Renesas RA8D2 MCU group. Built from the
command line with CMake and arm-none-eabi-gcc. Example apps for the
EK-RA8D2 evaluation kit (Renesas part 968-K7EKA8D2S01001BE) live under
`examples/ek_ra8d2/`, but the EVM is just one consumer of the HAL --
not the project's target.

## Target

| | |
|---|---|
| MCU | `R7KA8D2KFLCAC` |
| Primary core | Arm Cortex-M85 @ 1 GHz (Helium / MVE) |
| Secondary core | Arm Cortex-M33 @ 250 MHz |
| Code memory | 1 MB MRAM |
| SRAM | 2 MB (ECC) |
| Package | 289-BGA |
| Debugger | On-board SEGGER J-Link OB |

## Build and flash

The top-level Makefile auto-discovers every app under `examples/` and
forwards `make <app>` to the per-app Makefile. Build artifacts land in
`<app-dir>/build/<app>.{elf,hex,bin,map}`.

```sh
git clone https://github.com/bsikar/ra8d2-firmware.git
cd ra8d2-firmware
make help           # list top-level targets
make apps           # list every discovered app (grouped by tier)
make <app>          # cross-compile a specific app, e.g. `make blink`
make                # build the default app (RA_DEFAULT_APP, defaults to blink)
```

Per-app builds also work directly via `make -C <app-dir>`.

Flash the resulting `.hex` via the on-board J-Link OB:

```sh
bash scripts/flash.sh path/to/<app>.hex   # any .hex path
make -C <app-dir> flash                   # equivalent shortcut
```

`scripts/flash.sh` drives `JLinkExe -device R7KA8D2KF_CPU0 -if SWD` and
loads the `.hex` into MRAM at `0x02000000`. OpenOCD with any SWD probe
and an RA8D2 target config will flash the same artifact.

Debug entry points: `make -C <app-dir> ozone` (SEGGER Ozone GUI) and
`make -C <app-dir> debug` (gdb via JLinkGDBServer).

## Make targets

Run `make help` for the authoritative list. Current groups:

- Build: `make`, `make <app>`, `make apps`, `make clean`
- Format / lint: `make format`, `make check`, `make tidy`, `make ascii`
- Tests: `make test` (Linux native), `make test-docker` (macOS path),
  `make ctest`, `make coverage`, `make mcdc` / `make test-cov`
- Static analysis: `make scan-build`, `make scan-build-strict`,
  `make iwyu`, `make misra`, `make check-annotations`
- Hardware: `make hil`, `make stack-usage`
- Other: `make fuzz`, `make bench`, `make app-sizes`, `make audit-init`,
  `make docs`, `make dashboard`, `make version`

## Host unit tests

```sh
make test              # Linux: native ctest run
make test-docker       # macOS: same suite, run inside an Ubuntu container
```

The suite uses `tests/mocks/ra_sim_mmap.c` to install anonymous mmap
backings at every RA8D2 MMIO window so production driver code paths run
unmodified on the x86_64 host. On Apple Silicon the kernel rejects
`MAP_FIXED` at low addresses, so the macOS path goes through Docker.

## Repository layout

- `examples/<tier>/.../<app>/` -- self-contained applications (own
  `main.c`, boot files, linker script, Makefile, CMakeLists.txt). The
  `<tier>` directory groups apps by hardware-support category
  (`ek_ra8d2/` for the stock EVM, `_unsupported/` for apps needing
  extra hardware).
- `libs/ra_*` -- hand-written HAL, BSP, PALs, crypto, graphics, EPUB,
  NSC veneers.
- `libs/third_party/` -- vendored ThreadX X-Ware, NimBLE, Mbed TLS,
  litehtml, tinyxml2, etc. with integration shims.
- `src/secure_app/` -- TrustZone secure-side substrate.
- `tests/` -- host-side unit tests (native gcc/clang, not cross-compiled).
- `scripts/` -- flash, debug, format, tidy, pre-commit hook, utilities.
- `docs/reference/` -- committed datasheets and manuals.

## Pre-commit hook

`scripts/git/pre-commit` runs ASCII check, C23 pattern check,
clang-format, clang-tidy, `@since` tag check, copyright header check,
world-tag check, HUM-citation check, and cppcheck. Install with:

```sh
ln -s ../../scripts/git/pre-commit .git/hooks/pre-commit
```

## Key docs

- [`CLAUDE.md`](CLAUDE.md) -- project rules and conventions
- [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md) -- authoritative style guide
- [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md) -- architectural rings + TrustZone world tags
- [`docs/VENDOR_BLOBS.md`](docs/VENDOR_BLOBS.md) -- vendored third-party libraries
- [`docs/DEBUG.md`](docs/DEBUG.md) -- debugging workflow (Ozone, gdb, J-Link)
- [`docs/ROADMAP_DASHBOARD.md`](docs/ROADMAP_DASHBOARD.md) -- per-driver roadmap status
- [`docs/reference/ra8d2-hardware-user-manual.pdf`](docs/reference/ra8d2-hardware-user-manual.pdf) -- primary register reference (R01UH1065EJ)
- [`docs/reference/ek-ra8d2-v1-users-manual.pdf`](docs/reference/ek-ra8d2-v1-users-manual.pdf) -- EK-RA8D2 board user's manual
- [`CHANGELOG.md`](CHANGELOG.md) -- per-commit history

## License

MIT. See [`LICENSE.txt`](LICENSE.txt).

# ra8d2-firmware

Bare-metal firmware for the Renesas **RA8D2** MCU group, targeting the
**EK-RA8D2** evaluation kit (part number 968-K7EKA8D2S01001BE).

Personal in-house project by Brighton Sikarskie. No vendor FSP code is
checked in -- every `.c` / `.h` in `src/` and `libs/` is hand-written
against the RA8D2 Hardware User's Manual.

## Target

| | |
|---|---|
| MCU | `R7KA8D2KFLCAC` |
| Primary core | Arm Cortex-M85 @ 1 GHz (Helium / MVE) |
| Secondary core | Arm Cortex-M33 @ 250 MHz |
| Code memory | 1 MB MRAM |
| SRAM | 2 MB (ECC) |
| Package | 289-BGA (12 mm x 12 mm, 0.65 mm pitch) |
| Debugger | On-board SEGGER J-Link OB (RA4M2-hosted) |

## What's in the tree

| Layer | Where | Count |
|---|---|---:|
| Core utilities (no HW deps) | `libs/ra_core/` | 7 modules |
| HAL drivers | `libs/ra_hal/src/` | 60 |
| HAL public headers | `libs/ra_hal/inc/ra_*.h` | 65 |
| Register headers (hand-written) | `libs/ra_hal/inc/ra8d2_*_regs.h` | 58 |
| NSC veneers (TrustZone bridges) | `libs/ra_nsc/` | 7 modules |
| Net / USB PALs | `libs/ra_net_pal/`, `libs/ra_usb_pal/` | 2 |
| Per-app boot files (vector table + SystemInit + linker script) | `<app>/{vector_table,system_init,secure_exception,trustzone_init}.c` + `<app>/linker_script.ld` | each app |
| Secure-app code | `src/secure_app/` | 2 files |
| Application apps (each standalone) | `<app>/main.c` + per-app boot + linker | `blink/`, `blink_hal/` |
| Host unit tests | `tests/test_*.c` | 86 |

The HAL covers every peripheral the RA8D2 exposes: the full
graphics pipeline (CEU, DRW, GLCDC, MIPI-PHY/DSI/CSI, VIN), MRAM
flash programming, SRAM ECC, USB FS+HS device-mode pipes, ADC scan
modes, dual-core IPC, RSIP secure crypto, low-power modes,
voltage-detect, battery-backup, every timer family, every comm
interface (SCI / IIC / SPI / I3C / xSPI / SDHI / CANFD / Ethernet
agent + MAC + switch fabric), watchdog, reset cause introspection,
and on. See `docs/ROADMAP.md` for the per-driver status table.

## Quick start

```sh
make blink             # cross-compile blink/main.c
make -C blink flash    # flash blink/build/blink.hex via on-board J-Link OB
```

`make blink_hal` builds the HAL-based blink demo instead.

The blink demo toggles `P6_00` / `P6_01` / `P6_02` / `P3_03` /
`P10_07` at 1 Hz so whichever pin actually wires to a board LED on
your EK-RA8D2 lights up.

Each app is standalone; `cd blink && make` produces the same artifacts
as `make blink` from the repo root.

## Make targets

```
make             build the default app (blink)
make <app>       build a specific app, e.g. `make blink_hal`
make apps        list every discovered app
make -C <app> flash    flash that app via scripts/flash.sh
make -C <app> ozone    open SEGGER Ozone GUI debugger on that app's elf
make -C <app> debug    attach gdb via JLinkGDBServer on that app's elf
make test            host-compile + run unit tests (Linux native)
make test-docker     host-compile + run unit tests in Linux container
                     (use this on macOS where MAP_FIXED is blocked)
make tidy            run clang-tidy over the whole tree
make format          run clang-format in place
make check           run clang-format --dry-run
make docs            run doxygen
make ascii           verify ASCII-only source files
make clean           remove every app build dir and tests/build/
```

## Cross-build artifacts

After `make blink`, artifacts land in `blink/build/`:

```
blink/build/blink.elf    ELF with DWARF symbols (debugger target)
blink/build/blink.hex    Intel HEX (for flashers)
blink/build/blink.bin    Raw binary (zero-padded; rarely needed)
blink/build/blink.map    Linker map
```

`blink_hal` produces the equivalent files under `blink_hal/build/`.

## Flashing

The on-board RA4M2 J-Link OB on the EK-RA8D2 is recognised by
`SEGGER J-Link Software Pack` (install via `brew install --cask segger-jlink`
or from segger.com).

```sh
make -C blink flash             # uses scripts/flash.sh blink/build/blink.hex
make -C blink_hal flash         # ditto for blink_hal
```

The script invokes `JLinkExe -device R7KA8D2KF_CPU0 -if SWD -NoGui 1`
and loads the per-app `.hex` into MRAM at `0x02000000`.

## Debugging

```sh
make -C blink ozone     # SEGGER Ozone GUI -- best for HardFault triage
make -C blink debug     # gdb attached via JLinkGDBServer
```

The Ozone project file is committed at `scripts/ra8d2.jdebug` so
device + interface + ELF path are version-controlled. The default
ELF path it loads is `blink/build/blink.elf`; the per-app `make
ozone` target points the wrapper at the right elf for any other app.

## Host unit tests

```sh
make test              # Linux: native ctest run
make test-docker       # macOS: same suite, run inside an Ubuntu container
```

The test suite uses `tests/mocks/ra_sim_mmap.c` to install anonymous
mmap backings at every RA8D2 MMIO window, so the actual production
driver code paths run on the x86_64 host unmodified. On Apple Silicon
the kernel rejects `MAP_FIXED` at low addresses regardless of
`-pagezero_size`, so the macOS path goes through Docker.

## Lint, format, and pre-commit

```sh
make tidy              # clang-tidy with project .clang-tidy ruleset
make format            # clang-format --in-place
make check             # clang-format --dry-run -Werror
```

A pre-commit hook at `scripts/git/pre-commit` runs ASCII check, C23
pattern check, clang-format, clang-tidy, `@since` tag check,
copyright header check, world-tag check, HUM-citation check, and
cppcheck on every commit. Install with:

```sh
ln -s ../../scripts/git/pre-commit .git/hooks/pre-commit
```

## CI

`.github/workflows/firmware.yml` runs every gate on push to `main`
and on every pull request.

## Project conventions

| Topic | Doc |
|---|---|
| File-header Doxygen, naming, types, NASA Power-of-10, SOLID | [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md) |
| Architectural rings + TrustZone world tagging | [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md) |
| Layered architecture overview | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| Per-driver status / roadmap | [`docs/ROADMAP.md`](docs/ROADMAP.md) |
| HUM chapter -> page-range map | [`docs/reference/CHAPTER_MAP.md`](docs/reference/CHAPTER_MAP.md) |

CLAUDE.md (project root) summarises the most-violated style rules
for AI-assisted contributors; `docs/STYLE_GUIDE.md` is the
authoritative source.

## Reference material

Committed under [`docs/reference/`](docs/reference/):

- `ra8d2-datasheet.pdf` -- electrical specs, pin lists
- `ra8d2-hardware-user-manual.pdf` -- primary register reference (R01UH1065EJ)
- `ra8d2-technical-brief.pdf` -- high-level overview
- `ra8d2-high-temperature-operation.pdf` -- hi-temp application note

The HUM is also pre-split into per-page PDFs under
`docs/reference/ra8d2-hardware-user-manual/page-NNNN.pdf` so
`scripts/utils/cite_check.py` can validate every `/* HUM Ch X.Y "..."
p NNNN */` annotation in the source against the chapter's documented
page range.

## License

MIT. See `LICENSE.txt`.

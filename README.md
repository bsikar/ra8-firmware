# ra8-firmware

[![firmware](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml/badge.svg?branch=main)](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml)

Bare-metal firmware for the Renesas RA8D2 MCU group -- a hand-written HAL,
board-support layers, and a growing set of example apps, built from the command
line with CMake and `arm-none-eabi-gcc`. No vendor IDE artifacts and no Renesas
FSP code in the tree. The EK-RA8D2 evaluation kit is the reference board (apps
under `examples/ek_ra8d2/`), but it is just one consumer of the HAL, not the
target.

## Target hardware

| | |
|---|---|
| MCU | `R7KA8D2KFLCAC` (RA8D2 group) |
| Primary core | Arm Cortex-M85 @ 1 GHz (Helium / MVE) |
| Secondary core | Arm Cortex-M33 @ 250 MHz |
| Code memory | 1 MB MRAM |
| SRAM | 2 MB (ECC) |
| Package | 289-BGA |
| Debugger | On-board SEGGER J-Link OB |

## Quick start

The top-level `Makefile` auto-discovers every app under `examples/` and forwards
to its per-app build. Run `make help` for the authoritative, always-current list
of targets -- this README deliberately does not enumerate them.

```sh
git clone https://github.com/bsikar/ra8-firmware.git
cd ra8-firmware

make help          # every target, grouped and described
make apps          # list every discovered app, by tier
make <app>         # cross-compile one app, e.g. `make blink`
make               # build the default app
```

Artifacts land in `<app-dir>/build/<app>.{elf,hex,bin,map}`. Per-app builds also
work directly with `make -C <app-dir>`.

## Flash and debug

```sh
make -C <app-dir> flash    # program the .hex over the on-board J-Link OB
make -C <app-dir> ozone    # SEGGER Ozone GUI
make -C <app-dir> debug    # gdb via JLinkGDBServer
```

Flashing loads the `.hex` into MRAM over SWD; any SWD probe with an RA8D2 target
config works too. See [`docs/DEBUG.md`](docs/DEBUG.md) for the full workflow.

## Tests and checks

Host unit tests compile the real driver code against mocked MMIO and run
natively -- no board required:

```sh
make test          # host unit-test suite
```

These need a Linux toolchain; on macOS/Windows run them (and the other gates)
through the reproducible container -- see [Development environment](#development-environment).
`make help` lists the coverage, MC/DC, lint, and static-analysis targets.

## Repository layout

- `examples/<tier>/.../<app>/` -- self-contained apps (own `main.c`, boot files,
  linker script, build files). `<tier>` groups apps by hardware support
  (`ek_ra8d2/` for the stock EVM, `_unsupported/` for apps needing extra hardware).
- `libs/ra8_*` -- hand-written HAL, board layers, PALs, crypto, graphics, the
  e-reader stack, and TrustZone NSC veneers.
- `libs/third_party/` -- vendored SOUP (ThreadX/USBX/NetX, NimBLE, Mbed TLS,
  litehtml, ...) with thin integration shims.
- `src/` -- shared internals and the TrustZone secure-side substrate.
- `tests/` -- host-side unit tests (native gcc/clang, not cross-compiled).
- `scripts/` -- flash, debug, format, lint, the pre-commit hook, and utilities.
- `docs/` -- architecture, style, and the committed reference manuals.

## Development environment

A reproducible toolchain is provided as a VS Code **Dev Container**
(`.devcontainer/`): the pinned `arm-none-eabi-gcc`, clang tooling, and gates
that CI uses. `make ci` runs the full CI gate suite inside that same container,
so a red gate is caught locally before a push.

The pre-commit hook (`scripts/git/pre-commit`) runs the fast gates (ASCII, C23
patterns, clang-format/tidy, doc/citation/terminology checks). Install it with:

```sh
ln -s ../../scripts/git/pre-commit .git/hooks/pre-commit
```

Conventions (C23 style, Doxygen, architectural rings, TrustZone world tags) are
documented in the guides below and enforced by the gates.

## Documentation

- [`CLAUDE.md`](CLAUDE.md) -- project rules and conventions
- [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md) -- authoritative style guide
- [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md) -- architectural rings + TrustZone world tags
- [`docs/DEBUG.md`](docs/DEBUG.md) -- debugging workflow (Ozone, gdb, J-Link)
- [`docs/VENDOR_BLOBS.md`](docs/VENDOR_BLOBS.md) -- vendored third-party libraries
- [`docs/reference/`](docs/reference/) -- committed RA8D2 + EK-RA8D2 manuals (primary register reference)

## License

MIT. See [`LICENSE.txt`](LICENSE.txt).

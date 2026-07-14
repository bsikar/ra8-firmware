# ra8-firmware

[![firmware](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml/badge.svg?branch=main)](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml)

Bare-metal firmware for the Renesas RA8 MCU family. Two devices are supported
today, selected at build time: the RA8D2 and the RA8P1 (an RA8D2 with an
Ethos-U55 NPU). Building, testing, the CPU emulator, and the CI gates all run
from the command line.

## Getting started

```sh
git clone https://github.com/bsikar/ra8-firmware.git
cd ra8-firmware
make help
```

`make help` is the source of truth: it lists every target, grouped by what it
does. A few starting points:

```sh
make apps            # list every firmware app
make <app>           # build one app, e.g. make blink
make sim-<app>       # run an app on the CPU emulator, no board (e.g. make sim-blink)
```

Most workflows follow the same `make <verb>-<app>` shape, so once you know an app
name from `make apps` you can build it, run it in the emulator, flash it, or
debug it. The groups in `make help` cover:

- Build and preview (`make <app>`, `make sim-<app>`, the e-reader GUI)
- Local hardware over a J-Link on this machine (`make flash-<app>`, `make debug-<app>`, `make ozone-<app>`)
- Hardware-in-the-loop on a remote board rig (`make hil`)
- Quality and CI (`make ci`, `make test`, `make coverage`, `make mcdc`)

## Examples

Every app under `examples/` is self-contained and carries its own README. Most
run in the emulator with no hardware at all. Apps that target a specific board
live under [`examples/ek_ra8d2/`](examples/ek_ra8d2/README.md); apps that need
extra parts (motor driver, audio codec, and so on) live under
[`examples/_unsupported/`](examples/_unsupported/README.md).

## Development environment

A reproducible toolchain is provided as a VS Code Dev Container
(`.devcontainer/`) with the pinned compilers and tooling. `make ci` runs the
full CI gate suite inside that same container, so a red gate is caught locally
before a push.

Git hooks (formatting, lint, and the pre-commit gates) install themselves the
first time you run any `make` target; `make hooks` reinstalls them.

## Approach

Bare-metal, with a hand-written HAL over the chip register map. No Renesas FSP
code and no vendor IDE project files are committed; the FSP sources and the
datasheets are reference material only. The conventions the gates enforce (C23
style, Doxygen coverage, architectural rings, TrustZone world tags) are
documented in [`CLAUDE.md`](CLAUDE.md) and [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md).

## Documentation

- [`CLAUDE.md`](CLAUDE.md): project rules and conventions
- [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md): style guide
- [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md): architectural rings and TrustZone world tags
- [`docs/DEBUG.md`](docs/DEBUG.md): debugging workflow (Ozone, gdb, J-Link)
- [`docs/reference/`](docs/reference/): committed RA8 datasheets and manuals

## License

MIT. See [`LICENSE.txt`](LICENSE.txt).

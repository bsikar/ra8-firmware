# ra8-firmware

[![firmware](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml/badge.svg?branch=main)](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml)

Bare-metal firmware for the Renesas RA8 family (RA8D2 and RA8P1).

## Getting started

```sh
git clone https://github.com/bsikar/ra8-firmware.git
cd ra8-firmware
make help
```

`make help` lists every target. To start:

```sh
make apps          # list apps
make <app>         # build one, e.g. make blink
make sim-<app>     # run it on the emulator, no board
```

- Per app: `make flash-<app>`, `make debug-<app>`, `make ozone-<app>`
- Suites: `make hil`, `make ci`, `make test`, `make coverage`, `make mcdc`

## Examples

Each app under `examples/` is self-contained with its own README. Most need no
hardware. Board-specific apps are in
[`examples/ek_ra8d2/`](examples/ek_ra8d2/README.md); apps needing extra parts
are in [`examples/_unsupported/`](examples/_unsupported/README.md).

## Development

VS Code Dev Container (`.devcontainer/`) with the pinned toolchain; `make ci`
runs the gates in it. Git hooks install on the first `make`.

## Docs

- [`CLAUDE.md`](CLAUDE.md), [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md): rules and style
- [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md): rings and TrustZone worlds
- [`docs/DEBUG.md`](docs/DEBUG.md): debugging
- [`docs/reference/`](docs/reference/): datasheets and manuals

## License

MIT. See [`LICENSE.txt`](LICENSE.txt).

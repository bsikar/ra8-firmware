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
- Suites: `make hil`, `make ci` (or `make ci-native`), `make test`, `make coverage`, `make mcdc`

## Examples

Each app under `examples/` is self-contained with its own README. Most need no
hardware. Board-specific apps are in
[`examples/ek_ra8d2/`](examples/ek_ra8d2/README.md); apps needing extra parts
are in [`examples/_unsupported/`](examples/_unsupported/README.md).

## Development

VS Code Dev Container (`.devcontainer/`) with the pinned toolchain; `make ci`
runs the gates in it, `make ci-native` runs them without a container on Linux.
Every gate body lives once, in `scripts/ci.sh`, and each CI workflow step is a
thin `bash scripts/ci.sh --gate <name>` driver -- so local and CI cannot execute
different checks. Git hooks install on the first `make`.

## Hardware and flashing

`make sim-<app>` needs no hardware. To flash or debug a real board you need an
EK-RA8D2 (on-board J-Link) and the SEGGER J-Link tools. Rig settings (probe
serial, bench Pi) live in a gitignored `.env`; copy the template and fill in
what you use:

```sh
cp .env.example .env
make hil-find-jlink     # prints your J-Link serial to paste into .env
```

- Flash / debug a board cabled to your machine: `make flash-<app>`,
  `make debug-<app>`.
- The `make hil-*` targets drive the maintainer's bench rig (a Pi plus Tapo
  smart plugs); they will not work elsewhere without your own `.env` and rig.

**Bricked board?** `bash scripts/hil/dlm_reset_local.sh recover` mass-erases an
EK-RA8D2 wired to this machine and returns it to the factory default
(all-Secure, OEM_PL2). Run `... check` first to confirm the probe can reach it.

## Docs

- [`CLAUDE.md`](CLAUDE.md), [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md): rules and style
- [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md): rings and TrustZone worlds
- [`docs/DEBUG.md`](docs/DEBUG.md): debugging
- [`docs/reference/`](docs/reference/): datasheets and manuals

## License

MIT. See [`LICENSE.txt`](LICENSE.txt).

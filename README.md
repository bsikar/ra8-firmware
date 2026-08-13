# ra8-firmware

[![firmware](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml/badge.svg?branch=main)](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml)

Bare-metal firmware for the Renesas RA8 family (RA8D2 and RA8P1), plus a host
emulator that boots the real firmware images so none of it needs a board.

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
make emu-<app>     # run it on the emulator, no board
```

- Per app: `make flash-<app>`, `make debug-<app>`, `make ozone-<app>`
- Suites: `make hil`, `make ci` (or `make ci-native`), `make test`, `make coverage`, `make mcdc`

## No board? Run the firmware anyway

[`tools/ra8_emulator`](tools/ra8_emulator/README.md) boots the **real,
unmodified `.elf`** -- the same image that flashes to an EK-RA8D2 -- on an
emulated Cortex-M (Unicorn) over a modelled RA8D2 peripheral space. It shows
the GLCDC panel framebuffer beside a status sidebar carrying the three board
LEDs and live USB / UART / IRQ / touch state, so a display app draws its real
screen and a non-display app (blink, USB, UART, timers) is still watchable. A
click in the window enters the firmware through the genuine GT911 touch path;
typing feeds the console UART's receive path.

```sh
make emu-setup                 # macOS: provisions dependencies (requires Command Line Tools, not Xcode)
make emu-blink                 # live window: LEDs plus the USB/UART/IRQ sidebar
make emu-ereader_ui            # the e-reader chrome, drawn by the firmware itself
make emu-help                  # panels, profiling, the whole flag surface
```

The live window is macOS-only (Cocoa). Every other path -- boot, MMIO report,
console capture, frame dump -- also runs headless on Linux:

```sh
make blink
cmake -B tools/ra8_emulator/build -S tools/ra8_emulator
cmake --build tools/ra8_emulator/build -j
./tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_validated/hil/blink/build/blink.elf --ppm frame.ppm
```

It executes the actual bring-up and driver code, so it reproduces what the
silicon does -- including real firmware bugs a short bench run never reaches
(the `ra8_mstp` refcount saturation behind the AGT faults surfaced here first).
It models hardware *handshakes* rather than silicon timing, and some app
families are modelled further than others, so it complements the bench instead
of replacing it. CI leans on it: `make eil` boots every HIL app headless and
asserts its expectation, and `make emu-matrix` sweeps every example.

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

`make emu-<app>` needs no hardware. To flash or debug a real board you need an
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
- [`tools/ra8_emulator/README.md`](tools/ra8_emulator/README.md): the board emulator
- [`tools/README.md`](tools/README.md): the rest of the host tooling
- [`docs/reference/`](docs/reference/): datasheets and manuals

## What is being worked on

The tracker is a [project board](https://github.com/users/bsikar/projects/5)
that is **private** -- the link only opens for the maintainers. It holds no
work of its own: every card is one of this repository's
[issues](https://github.com/bsikar/ra8-firmware/issues), which are public and
are the real record. The board adds scheduling on top of them, sorting each
issue into a lane (`Needs you`, `Bench-blocked`, `Ready`, `In flight`,
`In review`, `Landed`) with a track and a priority.

So if the board link is closed to you, nothing is hidden: read the issues, and
the labels (`priority:`, `epic:`, `area:`, `effort:`, `needs-bench`) carry the
same information the board columns do. The lane rules agents work to are in
[`CLAUDE.md`](CLAUDE.md).

## License

MIT. See [`LICENSE.txt`](LICENSE.txt).

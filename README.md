# ra8-firmware

[![firmware](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml/badge.svg?branch=main)](https://github.com/bsikar/ra8-firmware/actions/workflows/firmware.yml)

A bare-metal firmware platform for the Renesas RA8 family (RA8D2 and RA8P1):
a hand-written HAL over the chip's register map, ports of the RTOS / USB /
networking / TLS stacks, and a host emulator that boots the real firmware
images so none of it needs a board. No Renesas FSP code lives here -- the FSP
sources and the Hardware User's Manual are reference material, and every line
under `libs/` and `src/` is written against the manual.

## Shape of the tree

```mermaid
graph TD
    APPS["apps/ -- proving products<br/>stand_alone/ereader, stand_alone/media_dl"]
    EX["examples/ -- per-library demos and HIL vehicles<br/>ek_ra8d2, ra8p1_foundation"]
    LIBS["libs/ra8_* -- content, UI, memory, I/O fabric, crypto"]
    PORT["port/ -- ThreadX, NetX Duo, USBX, LevelX, Mbed TLS, NimBLE"]
    HAL["libs/ra8_hal -- HUM-derived registers and drivers<br/>the only layer that touches MMIO"]
    CORE["libs/ra8_core -- pure C, no hardware, identical on the test host"]
    SIL["EK-RA8D2 / RA8P1 silicon"]
    EMU["tools/ra8_emulator -- the same .elf, no board"]

    APPS --> LIBS
    EX --> LIBS
    LIBS --> PORT
    LIBS --> HAL
    PORT --> HAL
    HAL --> CORE
    HAL --> SIL
    HAL --> EMU
```

The three tiers are deliberate. `libs/`, `port/`, the emulator and the CI
machinery are the **platform** -- the reason the repo exists. `examples/` are
per-library demos that double as the hardware-in-the-loop vehicles. `apps/`
holds **proving products**, each structured as if it were its own repository
and consuming `libs/` the way an outside consumer would; `libs/` never imports
from `apps/`. Alongside them sit `tests/` (host unit tests, never
cross-compiled), `tools/`, `scripts/`, `infra/` (the declared CI fleet),
`coprocessor/` (ESP32-C6 companion firmware) and `docs/`.

## Running it

```sh
make apps          # list the firmware apps
make <app>         # cross-compile one, e.g. make blink
make emu-<app>     # boot it on the emulator, no board
make test          # host unit tests
```

`make help` is the grouped target reference; hooks install on the first `make`.
Per app there is also `make flash-<app>`, `make debug-<app>`, `make ozone-<app>`.

## The emulator

[`tools/ra8_emulator`](tools/ra8_emulator/README.md) boots the unmodified
cross-compiled `.elf` -- the same image that flashes to a board -- on an
emulated Cortex-M over a modelled RA8D2 peripheral space, and shows the GLCDC
framebuffer beside the board LEDs and live USB / UART / IRQ / touch state. The
live window is macOS-only (Cocoa, provisioned by `make emu-setup`); booting,
MMIO reporting, console capture and frame dumps run headless everywhere.

Because it runs the real binary through the real bring-up code, it reproduces
firmware bugs a short bench run never reaches. It models hardware *handshakes*
rather than silicon timing, and some peripherals are modelled further than
others, so it complements the bench instead of replacing it. CI leans on it:
`make eil` boots every HIL app headless and asserts its expectation, and
`make emu-matrix` sweeps every example.

## Hardware

The bench board is an **EK-RA8D2**: Cortex-M85 at 1 GHz plus Cortex-M33 at
250 MHz, 1 MB MRAM, 1.6 MB ECC SRAM, 64 MB Octo-SPI flash and 64 MB SDRAM, a
7.0-inch 1024x600 parallel TFT, an OV5640 camera, and an on-board J-Link OB.
Flashing and debugging need the SEGGER J-Link tools.

Rig settings (probe serial, bench Pi) live in a gitignored `.env`; copy
`.env.example` and run `make hil-find-jlink` for the serial. The `make hil-*`
targets drive the maintainer's bench rig and will not work elsewhere without
your own rig.

**Bricked board?** `bash scripts/hil/dlm_reset_local.sh recover` mass-erases an
EK-RA8D2 cabled to this machine and returns it to the factory default
(all-Secure, OEM_PL2); run `... check` first to confirm the probe reaches it.

## CI

Every gate body lives once, in `scripts/ci.sh`, and each workflow step is a thin
`bash scripts/ci.sh --gate <name>` driver, so local and CI cannot run different
checks. `make ci` runs them all in the pinned devcontainer; `make ci-native` is
the same set without a container runtime, which is the supported path on Linux.

## Docs

| | |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | how the firmware is put together |
| [`docs/MODULES.md`](docs/MODULES.md) | a tour of every library under `libs/` |
| [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md) | architectural rings and TrustZone worlds |
| [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md) | the authoritative style rules |
| [`docs/DEBUG.md`](docs/DEBUG.md) | debugging on the board |
| [`docs/HARDWARE_BRINGUP.md`](docs/HARDWARE_BRINGUP.md) | bring-up notes from the bench |
| [`docs/reference/`](docs/reference/) | the datasheet and Hardware User's Manual |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | how to land a change |

## Tracker

The [issues](https://github.com/bsikar/ra8-firmware/issues) are the public
record. A private project board sorts them into lanes on top; if the board link
does not open for you, nothing is hidden -- the `priority:`, `epic:`, `area:`
and `needs-bench` labels carry the same information.

## License

MIT. See [`LICENSE.txt`](LICENSE.txt).

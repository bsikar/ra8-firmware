# ra8d2-firmware

[![firmware](https://github.com/bsikar/ra8d2-firmware/actions/workflows/firmware.yml/badge.svg?branch=main)](https://github.com/bsikar/ra8d2-firmware/actions/workflows/firmware.yml)

Bare-metal firmware for the Renesas **RA8D2** MCU group, targeting the
**EK-RA8D2** evaluation kit (part number 968-K7EKA8D2S01001BE).

Personal in-house project by Brighton Sikarskie. No vendor FSP code is
checked in -- every `.c` / `.h` under `src/` and `libs/ra_*` (excluding
`libs/third_party/`) is hand-written against the RA8D2 Hardware User's
Manual.

**Status:** Tests: 135/135 passing | clang-tidy: clean | Build: 36/36 example apps cross-compile

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

## What's included

- **HAL coverage** -- 45/45 roadmap drivers complete (93 `.c` files
  total in `libs/ra_hal/src/`), spanning the full graphics pipeline (CEU/DRW/GLCDC/MIPI-DSI/CSI/VIN),
  MRAM programming, SRAM ECC, USB FS+HS device pipes, ADC, dual-core
  IPC, RSIP crypto, every timer / SCI / IIC / SPI / I3C / xSPI / SDHI /
  CANFD / Ethernet / watchdog block.
- **Board support** -- `libs/ra_board_ek_ra8d2/` BSP wires the HAL to
  the EK-RA8D2 eval board (LEDs, switches, PMOD headers, MIPI display).
- **ThreadX X-Ware** -- ThreadX, NetX Duo, FileX, LevelX, GUIX, USBX
  vendored under `libs/third_party/` with hand-written integration shims.
- **Dual networking stack** -- lwIP and NetX Duo both wired through
  `libs/ra_net_pal/`; pick per-app.
- **Bluetooth** -- Apache NimBLE host stack vendored under
  `libs/third_party/nimble/` with HCI shim.
- **Crypto** -- Mbed TLS + tf-psa-crypto (`libs/ra_psa_crypto/`,
  `libs/ra_tls/`) backed by RSIP secure-element NSC veneers.
- **EPUB ereader** -- `libs/ra_epub/` ZIP+XHTML+CSS reflow stack on top
  of `libs/ra_gfx/` and litehtml/tinyxml2.
- **Secure / TrustZone** -- ring-tagged secure substrate in
  `src/secure_app/` plus NSC veneers in `libs/ra_nsc/`.

## Quick links

- [`CHANGELOG.md`](CHANGELOG.md) -- per-commit history of what shipped
- [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md) -- authoritative style guide
- [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md) -- architectural rings + TrustZone world tags
- [`docs/VENDOR_BLOBS.md`](docs/VENDOR_BLOBS.md) -- vendored third-party libraries and how they're integrated
- [`docs/DEBUG.md`](docs/DEBUG.md) -- debugging workflow (Ozone, gdb, J-Link)
- [`docs/ROADMAP_DASHBOARD.md`](docs/ROADMAP_DASHBOARD.md) -- per-driver roadmap status
- [`docs/reference/ek-ra8d2-v1-users-manual.pdf`](docs/reference/ek-ra8d2-v1-users-manual.pdf) -- EK-RA8D2 board user's manual
- [`docs/reference/ra8d2-hardware-user-manual.pdf`](docs/reference/ra8d2-hardware-user-manual.pdf) -- primary register reference (R01UH1065EJ)

## Getting started

```sh
git clone https://github.com/bsikar/ra8d2-firmware.git
cd ra8d2-firmware
make blink                                   # cross-compile examples/blink
bash scripts/flash.sh build/blink/blink.hex  # flash via on-board J-Link OB
```

Per-app builds also work directly: `cd examples/blink && make`.

`scripts/flash.sh` drives `JLinkExe -device R7KA8D2KF_CPU0 -if SWD` and
loads the `.hex` into MRAM at `0x02000000`. OpenOCD is a supported
alternative -- the `.elf` and `.hex` artifacts are toolchain-agnostic, so
any SWD probe driven by OpenOCD with an RA8D2 target config will flash
the same image.

## Make targets

```
make             build the default app (blink)
make <app>       build a specific app, e.g. `make blink_hal`
make apps        list every discovered app under examples/
make -C examples/<app> flash    flash that app via scripts/flash.sh
make -C examples/<app> ozone    open SEGGER Ozone GUI debugger
make -C examples/<app> debug    attach gdb via JLinkGDBServer
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

## Host unit tests

```sh
make test              # Linux: native ctest run
make test-docker       # macOS: same suite, run inside an Ubuntu container
```

The suite uses `tests/mocks/ra_sim_mmap.c` to install anonymous mmap
backings at every RA8D2 MMIO window so production driver code paths run
unmodified on the x86_64 host. On Apple Silicon the kernel rejects
`MAP_FIXED` at low addresses regardless of `-pagezero_size`, so the
macOS path goes through Docker.

## Pre-commit hook

`scripts/git/pre-commit` runs ASCII check, C23 pattern check,
clang-format, clang-tidy, `@since` tag check, copyright header check,
world-tag check, HUM-citation check, and cppcheck on every commit.
Install with:

```sh
ln -s ../../scripts/git/pre-commit .git/hooks/pre-commit
```

## CI

`.github/workflows/firmware.yml` runs every gate on push to `main` and
on every pull request.

## License

MIT. See [`LICENSE.txt`](LICENSE.txt).

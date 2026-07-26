# ESP32-C6 wireless co-processor architecture

## Summary

The EK-RA8D2 has no on-chip radio. Wi-Fi and Bluetooth are provided by an
**ESP32-C6** wired to the RA8D2 as a **wireless co-processor**. The C6 runs
Espressif's esp-hosted-mcu `network_adapter` firmware; the RA8D2 is the
**host**. A SPI transport carries the esp-hosted framing between them.

This document is the design-level architecture. Two concerns are deliberately
split out:

- The reproducible **C6 firmware build recipe** (pinned versions, config,
  build/flash scripts) lives in [`../../c6_firmware/`](../../c6_firmware/).
- The **SOUP qualification** for the esp-hosted-mcu firmware lives in
  [`../SOUP/esp-hosted.md`](../SOUP/esp-hosted.md).

## Roles and code ownership

| Side | Device | Software |
|------|--------|----------|
| Host | RA8D2 (Cortex-M85) | First-party RA8 host driver (a follow-on -- see below) |
| Co-processor | ESP32-C6 | Espressif esp-hosted-mcu `network_adapter`, unmodified SOUP |

**Zero first-party code runs on the C6.** The whole C6 image is upstream
esp-hosted-mcu, built from a pinned commit. Nothing project-authored is flashed
to the C6 and nothing from the C6 is linked into the RA8D2 firmware binary.

## Transport

esp-hosted-mcu multiplexes Wi-Fi data, Bluetooth HCI, and a control channel
over a single physical link. This project uses the **SPI** transport
(`CONFIG_ESP_SPI_HOST_INTERFACE`), with the RA8D2 as the SPI controller and the
C6 as the SPI peripheral.

Two out-of-band GPIOs pace the link:

- **DATA_READY** -- the C6 raises it to tell the host it has data to send.
- **HANDSHAKE** -- the C6 raises it when it is ready for the next transaction.

### Pin map (ESP32-C6 GPIO numbers)

Inclusive signal names; the pins are the single source of truth in
[`../../c6_firmware/pins.env`](../../c6_firmware/pins.env).

| Signal | ESP32-C6 GPIO | Direction |
|--------|---------------|-----------|
| CS (Chip Select) | GPIO0 | host -> C6 |
| COPI (Controller Out) | GPIO1 | host -> C6 |
| CIPO (Controller In) | GPIO2 | C6 -> host |
| SCK (clock) | GPIO3 | host -> C6 |
| DATA_READY | GPIO4 | C6 -> host |
| HANDSHAKE | GPIO6 | C6 -> host |
| RESET | disconnected | n/a |

RESET is left disconnected (`-1`) in this bring-up: the C6 is reset by power
cycling. A future revision may wire a host-driven reset line so the host can
recover the C6 without a power cycle.

## Boot and reset

1. Power-on: the C6 boots its own bootloader and starts the esp-hosted-mcu
   `network_adapter` app from flash (a one-time flashing step, see
   `c6_firmware/flash.sh`).
2. The RA8D2 host driver opens the SPI link and completes the esp-hosted
   handshake.
3. From then on the host issues Wi-Fi / Bluetooth / control requests and the
   C6 services them.

Because the C6 firmware is flashed once and independently, an RA8D2 firmware
update does not touch the C6, and vice versa.

## RA8-side host driver (follow-on)

The host-side driver that speaks the esp-hosted protocol over the RA8D2 SPI
peripheral is **not** part of the current change; that change codifies only the
C6 firmware build recipe and the SOUP qualification. The driver is a separate
follow-on and, when added, will:

- Present a single integration boundary (one RA8 module) for all C6 access, so
  the co-processor is never reached from application code directly.
- Reuse the existing `ra8_io` SPI bus facade for the physical transport.
- Carry its own host unit tests and MC/DC vectors under `tests/`.

## Why a co-processor rather than an on-chip radio

The RA8D2 has no integrated radio; a companion connectivity device is the only
route to Wi-Fi / Bluetooth on this board. esp-hosted-mcu is Espressif's
supported, proven-in-use solution for exactly this host-plus-co-processor
topology, which is why it is accepted as SOUP rather than reimplemented.

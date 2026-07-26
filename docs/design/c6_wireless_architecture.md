# ESP32-C6 wireless co-processor architecture

## Summary

The EK-RA8D2 has no on-chip radio. Wi-Fi and Bluetooth are provided by an
**ESP32-C6** wired to the RA8D2 as a **wireless co-processor**. The C6 runs
Espressif's esp-hosted-mcu `network_adapter` firmware; the RA8D2 is the
**host**. A SPI transport carries the esp-hosted framing between them.

This document is the design-level architecture. Two concerns are deliberately
split out:

- The reproducible **C6 firmware build recipe** (pinned versions, config,
  build/flash scripts) lives in [`../../coprocessor/esp32c6/`](../../coprocessor/esp32c6/).
- The **SOUP qualification** for the esp-hosted-mcu firmware lives in
  [`../SOUP/esp-hosted.md`](../SOUP/esp-hosted.md), and for the vendored
  host-side driver in [`../SOUP/esp-hosted-host.md`](../SOUP/esp-hosted-host.md).

Both sides of the link are the same upstream project at the same pinned
commit: the C6 runs the peripheral-side firmware, the RA8D2 links the host
driver, and matching protocol version 2.12.11 is what makes them speak.

## Roles and code ownership

| Side | Device | Software |
|------|--------|----------|
| Host | RA8D2 (Cortex-M85) | Upstream esp-hosted host driver (vendored SOUP) + a first-party RA8 port (a follow-on -- see below) |
| Co-processor | ESP32-C6 | Espressif esp-hosted-mcu `network_adapter`, unmodified SOUP |

**Zero first-party code runs on the C6.** The whole C6 image is upstream
esp-hosted-mcu, built from a pinned commit. Nothing project-authored is flashed
to the C6, and the C6 image itself is never linked into the RA8D2 firmware
binary -- the two are separate images that meet only on the wire.

The RA8D2 side is a mix by design: the protocol driver is the *same upstream
project* (vendored at `libs/third_party/esp-hosted/`, so the framing and RPC
encoding cannot drift from the peripheral side), while everything that touches
RA8D2 hardware -- the SPI transfers, GPIO, timers, tasks and memory -- is
first-party port code held to the full project bar.

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
[`../../coprocessor/esp32c6/pins.env`](../../coprocessor/esp32c6/pins.env).

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
   `coprocessor/esp32c6/flash.sh`).
2. The RA8D2 host driver opens the SPI link and completes the esp-hosted
   handshake.
3. From then on the host issues Wi-Fi / Bluetooth / control requests and the
   C6 services them.

Because the C6 firmware is flashed once and independently, an RA8D2 firmware
update does not touch the C6, and vice versa.

## RA8-side host driver (follow-on)

The upstream host driver source **is now vendored** at
`libs/third_party/esp-hosted/` (host driver + shared protocol at commit
`949bb30`, with the upstream ESP-IDF/FreeRTOS port deliberately left out).
Nothing compiles it yet: the driver includes port headers by name, so it
cannot build until the port exists.

The **first-party port and the build wiring are the follow-on change**. That
port supplies the ten `port_esp_hosted_host_*.h` header contracts, fills the
72-entry `hosted_osi_funcs_t` vtable, and defines the handful of link-time
symbols the vendored core leaves undefined -- all enumerated in
[`../SOUP/esp-hosted-host.md`](../SOUP/esp-hosted-host.md). When added it
will:

- Present a single integration boundary (one RA8 module) for all C6 access, so
  the co-processor is never reached from application code directly.
- Reuse the existing `ra8_io` SPI bus facade for the physical transport.
- Carry its own host unit tests and MC/DC vectors under `tests/`.

## Why a co-processor rather than an on-chip radio

The RA8D2 has no integrated radio; a companion connectivity device is the only
route to Wi-Fi / Bluetooth on this board. esp-hosted-mcu is Espressif's
supported, proven-in-use solution for exactly this host-plus-co-processor
topology, which is why it is accepted as SOUP rather than reimplemented.

### Why not a first-party radio driver

The C6 Wi-Fi MAC/PHY is not documented in Espressif public datasheets or the C6
technical reference manual -- only the wired peripherals are. Every production
Wi-Fi / Bluetooth stack for the part (ESP-IDF, Zephyr, NuttX, the Rust esp-radio
crates) links the same Espressif binary blobs (libpp, libnet80211, libphy,
libcoexist, and the BLE controller library); there is no open reimplementation
of the radio, and the reverse-engineering efforts that do exist support open
networks only, on the classic Xtensa ESP32, and still need the blob to bring up
the RF front end. A hand-written first-party driver for the one peripheral the
C6 exists to provide is therefore not achievable. The blobs are also what the
module FCC modular grant rests on, so shipping Espressif stock firmware keeps
that grant intact.

### Why esp-hosted-mcu over the alternatives

- **A-la-carte ESP-IDF components** do not stand alone; they drag Kconfig, the
  IDF Python environment, FreeRTOS and inter-component dependency resolution, so
  consuming the SDK as libraries does not work at that grain.
- **A full ESP-IDF application on the C6** is warranted only if substantial
  first-party logic must run next to the radio. Here the C6 jobs (radio, OTA
  ingress) are all appliance jobs, so that path buys a second toolchain and
  image format for no benefit.
- **ESP-AT** terminates TCP/IP on the C6, bypassing the RA8-side network stack
  and its TLS posture, offers no host-pushed OTA, and its C6 line is stalled.

Running the whole co-processor firmware as one pinned SOUP artifact keeps every
line the project compiles under its own rules, turns the SOUP boundary into a
documented wire protocol rather than a linker boundary, and preserves the FCC
modular grant.

## Primary sources

- esp-hosted-mcu: https://github.com/espressif/esp-hosted-mcu
- Component registry: https://components.espressif.com/components/espressif/esp_hosted
- Radio-less host precedent (ESP32-P4 companion): https://www.espressif.com/en/news/ESP32-P4
- Wi-Fi blob and license: https://github.com/espressif/esp32-wifi-lib
- Open-driver reality: https://github.com/esp32-open-mac/esp32-open-mac
- Field-recovery flasher: https://github.com/espressif/esp-serial-flasher

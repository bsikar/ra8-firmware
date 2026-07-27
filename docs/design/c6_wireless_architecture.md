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

Two out-of-band GPIOs pace the link. Their **idle levels differ**, which is
easy to get wrong and was got wrong here (see the side-band section below):

- **DATA_READY** -- idles **low**. The C6 raises it only while its transmit
  queue holds a frame, so on a healthy but quiet link this pin sits low
  indefinitely. A freshly-booted C6 is the exception: it holds DATA_READY high
  for its queued `ESPInit` event <!-- LEGACY-OK: upstream log tag --> until the
  first transaction drains it, and then it stays low.
- **HANDSHAKE** -- idles **high** once the transport is armed. With this
  image's `CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y`, the C6 drops it from its
  chip-select edge interrupt and re-raises it when the next transaction is
  queued, so it pulses low once per transaction. Bench-observed on every
  transfer: `pre=1 mid=0 post=1`.

The consequence for any bring-up tool: HANDSHAKE can be identified by watching
it move, and DATA_READY cannot.

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

### Pin map (RA8D2 side, Pmod1 / J26) -- bench-proven 2026-07-27

The C6 is soldered to Pmod1 (J26). Which MCU pin carries each J26 signal is
**not** fixed: EK-RA8D2 v1 UM Rev 1.01 Table 17 p 26 shows pins J26-1..J26-4
are muxed on the board, and Table 18 p 26 selects the mux position from
SW4-1 / SW4-2.

Every row below was scope-qualified end to end at the J26 hole, and the link
then came up at SPI mode 3 / 1 MHz with zero bad checksums. The same map is
machine-readable in
[`../../coprocessor/esp32c6/pins.env`](../../coprocessor/esp32c6/pins.env)
(`RA8_PIN_*` / `RA8_J26_*`), which is the source of truth.

| J26 | Signal (C6 GPIO) | RA8D2 pin, Pmod1 **SPI** position | RA8D2 pin, UART position |
|-----|------------------|-----------------------------------|--------------------------|
| 1 | CS (GPIO0) | `P804` (SS2/IRQ14) | `P800` (CTS2) |
| 2 | COPI (GPIO1) | `P801` (COPI2/TXD2) | `P801` |
| 3 | CIPO (GPIO2) | `P802` (CIPO2/RXD2) | `P802` |
| 4 | SCK (GPIO3) | `P803` (SCK2) | `P804` (RTS2) |
| 7 | HANDSHAKE (GPIO6) | `P006` (IRQ11-DS) | `P006` |
| 8 | DATA_READY (GPIO4) | `P402` | `P402` |
| 9 | *not connected* (future EN / reset) | `P412` | `P412` |
| 10 | *not connected* | `P413` | `P413` |

J26-5 and J26-11 are ground; there is no 3V3 wire, as the C6 is self-powered
over its own USB. J26-9 is the hole reserved for the host-driven reset line
that `C6_PIN_RESET=-1` records as absent.

The controller is **SCI2 in Simple-SPI mode** (`k_ra8_board_pmod1_sci_channel`),
with the chip-select owned as a GPIO so one assertion spans the whole
1600-byte esp-hosted frame. Board symbols: `k_ra8_board_pmod1_spi_*` and
`k_ra8_board_pmod1_irq` / `_reset` / `_gpio_a` / `_gpio_b`.

Note the trap in the UART position: J26-1 becomes `P800` and J26-4 becomes
`P804`, so a controller that drives `P804` as chip-select is really feeding
the C6's *clock* pin, and the C6 never sees a chip-select at all.

### Required SW4 DIP positions

| Switch | Required | Meaning (UM Table 3 p 16, Table 18 p 26) |
|--------|----------|------------------------------------------|
| SW4-1 | OFF | Pmod1 Mode Select 1 |
| SW4-2 | OFF | Pmod1 Mode Select 2; OFF+OFF selects SPI |
| SW4-3 | **ON** | Octo-SPI Inactive -- frees `P801`..`P804` for Pmod1 |
| SW4-4 | **OFF** | Arduino / mikroBUS inactive (SW4-3 ON + SW4-4 ON is invalid) |

**This bank is the single highest-risk setting on the board for this link.**
With SW4-4 ON and SW4-3 OFF the Octo-SPI flash owns the Pmod1 SPI pins and the
U6 / U9 bus switches stay open, so J26-1..J26-4 are not electrically connected
to the MCU -- while the MCU, the board and the C6 all appear perfectly
healthy. That misreading, not a wiring fault, was the whole 2026-07-26 outage.

SW4-4 OFF also takes the Arduino and mikroBUS connectors offline, so the
LSM6DSO IMU Click cannot be used at the same time as the C6 link. That is a
genuine board-level trade-off, not an oversight.

SW4-3 is a hardware-only analog mux: the U15 PI4IOE5V6408 expander can sense
and override the other SW4 lines, but a whole-output-space sweep (issue #44,
recorded on `ra8_board_io_expander_set_octospi_active`) established that its
GPIOs are not in the Octo-SPI path.

### Side-band assignment: resolved

`P006` carries HANDSHAKE and `P402` carries DATA_READY, established on the
bench by `examples/ek_ra8d2/hw_pending/c6_spi_probe` from the C6's own
behaviour rather than from a wiring note. The two took different evidence,
for the reason given in the transport section above:

- **HANDSHAKE (`P006`)** was identified by *motion*, and abundantly: it tracks
  the chip-select edge on every transaction (5 votes in the run), and the
  probe's chip-select hunt provokes that edge without needing a clock or a
  payload.
- **DATA_READY (`P402`)** was identified from its *level history* plus the
  physical qualification of J26-8. It read high while the C6 still held its
  queued boot event, then low from the first completed transaction onward and
  for the rest of the run -- exactly the profile of a DATA_READY whose
  transmit queue has drained, and the only side-band pin with it.

That second identification was nearly missed, and the near-miss is the
interesting part. DATA_READY transitions **once per boot** and then sits
still, and that single transition falls outside any transaction's sampling
window, so a rule that counts pre/mid/post transitions within a transaction
scores it zero forever. The probe's first heuristic did exactly that, read the
stillness as absence, and named the floating, unconnected `P413` instead on a
single noise sample.

The probe now settles the question electrically rather than by catching that
one transition: `c6_probe_pull_contest` reads each side-band pin with the
RA8D2's internal pull-up engaged, and a pin still reading low is being sunk by
a driver on the far end -- something no floating pin can imitate, requiring no
cooperation from the C6 and no lucky timing. That mechanism has not yet been
exercised on hardware; the map above does not rest on it. See the probe's
README for the full record, including the superseded 2026-07-26 diagnosis.

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

Separately, the first-light bench instrument for the *raw* link is
`examples/ek_ra8d2/hw_pending/c6_spi_probe`. It hand-decodes the payload
header from the same pinned upstream spec, but it is deliberately an app
rather than a driver: its job is to prove the wire, resolve the Pmod1 mux
position and identify the side-band pins, not to become the transport.

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

# c6_spi_probe -- first-light probe for the ESP32-C6 esp-hosted SPI link

Bring-up instrument for the raw SPI link between the RA8D2 and the ESP32-C6
wireless co-processor soldered to Pmod1 (J26). It opens the link, hand-decodes
the esp-hosted payload header out of the received bytes, and prints a single
PASS/FAIL verdict line on the board console.

It also answers two questions that nothing in this tree recorded:

1. **Which MCU pin the board's Pmod1 mux has wired to the C6 chip-select.**
   EK-RA8D2 v1 UM Table 18 p 26 makes J26-1 either `P804` (SPI position) or
   `P800` (UART position), and UM Table 3 p 16 adds SW4-3, which frees those
   pins from the on-board Octo-SPI flash only when it is ON.
2. **Which Pmod1 side-band pin carries DATA_READY and which carries
   HANDSHAKE** -- the C6 GPIO4 / GPIO6 outputs, whose RA8-side landing pins
   were never written down.

## Why `hw_pending`

`ra8_emulator` does not model an ESP32-C6 on Pmod1, so this app cannot be
gated by the SIL suite, and it is deliberately absent from the HIL suite
(no `hil.conf`) until the bench blocker below is cleared. It is a bench
instrument, run by hand.

## What it does

| Phase | What it proves |
|-------|----------------|
| Side-band read | Levels of all four Pmod1 side-band pins (P006 / P402 / P412 / P413) as no-pull inputs. No internal pull is used: the C6 drives HANDSHAKE and DATA_READY push-pull, and a pull-up would make an unconnected pin look asserted. |
| Muxed-net wire test | Drives each of P800/P801/P802/P803/P804 high, releases it to a no-pull input and samples, then repeats driving low. A net with nothing on it holds the driven level on its own capacitance; a terminated net snaps back. |
| Chip-select hunt | Asserts each muxed-net candidate in turn and watches the side-band pins. The C6 image sets `CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y`, so a live C6 drops HANDSHAKE on the chip-select falling edge -- **no clock and no payload are needed** to find both the chip-select pin and the HANDSHAKE pin. |
| SPI mode sweep | Opens SCI2 Simple-SPI as controller at 1 MHz and clocks four full 1600-byte esp-hosted transactions per SPI mode, starting at mode 3 (what the C6 image is built with) and falling back through 0/1/2. Each frame's header is decoded and its checksum recomputed. |

PASS requires at least one received frame with recognisable esp-hosted
structure: either a real frame (`offset == 12`, `1 <= len <= 1588`,
`if_type < 8`, checksum verified) or the C6's idle filler frame (`if_type ==
ESP_MAX_IF`, `if_num == 0xF`, `len == 0`). Both are trivially distinguishable
from a dead bus reading all-zero or all-ones.

## Protocol source of truth

Nothing from esp-hosted-mcu is vendored here. Every protocol constant is
hand-decoded from the pinned upstream tree (commit `949bb30`, firmware
`2.12.11`, see `coprocessor/esp32c6/pins.env`) and cites the upstream file it
came from in its Doxygen block:

- `docs/spi_full_duplex.md` -- transaction rules, side-band semantics, the
  5 MHz evaluation-clock recommendation this app starts an order of magnitude
  below.
- `common/esp_hosted_header.h` -- the twelve-byte `struct esp_payload_header`.
- `common/transport/esp_hosted_transport.h` -- 1600-byte transport buffer,
  interface identifiers, `compute_checksum()`.
- `host/drivers/transport/spi/spi_drv.c` -- reference host pacing, the idle
  filler frame the host sends, and the receive-validation rules mirrored by
  `c6_probe_classify()`.
- `slave/main/spi_slave_api.c` <!-- LEGACY-OK: upstream esp-hosted-mcu path; our term is peripheral-side -->
  -- what the C6 actually drives on HANDSHAKE and
  DATA_READY, and the idle filler frame it sends back.

Link parameters taken from the C6's own generated `sdkconfig` (the image in
`coprocessor/esp32c6/`): `CONFIG_ESP_SPI_MODE=3` (CPOL 1 / CPHA 1),
`CONFIG_ESP_SPI_CHECKSUM=y`, `CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y`, MSB-first,
8-bit frames, 1600 bytes per transaction.

## Build and run

```sh
make c6_spi_probe                      # from the repo root
bash scripts/hil/flash.sh c6_spi_probe # program + release from reset
```

Console: J-Link OB VCOM, 115200 8N1.

## Bench status: the C6 is NOT wired to the probed Pmod1 pins (2026-07-26)

The probe works. The link does not, and the cause is neither the board mux nor
the co-processor: **nothing on the ESP32-C6 is on the same net as any Pmod1 pin
this app can reach.**

Two facts were established on the bench and each rules out one earlier suspect:

- **The SW4 mux is correct.** Confirmed against the board: SW4-1 OFF, SW4-2 OFF
  (Pmod1 SPI position, UM Table 18 p 26) and SW4-3 ON (Octo-SPI inactive, UM
  Table 3 p 16). The Pmod1 SPI group *is* routed to J26.
- **The C6 is alive, armed, and correctly configured.** Its console shows a
  clean boot of esp-hosted-mcu 2.12.11, `Transport used :: SPI only`,
  `SPI Ctrl:1 mode: 3`, and the expected GPIO assignment <!-- LEGACY-OK: quoting the co-processor's verbatim console line -->
  (`CLK:3 MOSI:1 MISO:2 CS:0 HS:6 DR:4`, upstream's own signal names for SCK / COPI / CIPO), plus the queued boot event `event ESPInit` <!-- LEGACY-OK: upstream log tag -->
  waiting for a host.

### The measurement that settles it

The C6 drives HANDSHAKE and DATA_READY as GPIO outputs: both are necessarily
low through a reset and high again once the transport arms. So resetting the
C6 *in the middle* of a probe run must move any side-band pin that is really
on its net.

The C6 was reset mid-run (its boot banner captured as proof) while the probe
sampled all four Pmod1 side-band pins. Across **54 samples spanning that
reset**, the reading never changed even once:

```
### resetting C6 now (mid-run) ###
### C6 reset issued; boot log bytes: 4104 ###
=== distinct side-band readings seen across the whole run ===
     54 P006=1 P402=1 P412=0 P413=1
=== C6 confirmed to have rebooted mid-run? ===
1
```

A pin wired to a C6 output cannot sit still through that. All four did.

### Corroborating evidence from the same runs

- **Controller-in reads all-`0xFF`** on every one of the sixteen full-size
  transactions, in all four SPI modes. The C6 holds its controller-in line
  with an internal pull-down, so a *connected* C6 -- idle or not -- would read
  all-`0x00`. `0xFF` is an unterminated RA8 input, not a peripheral.
- **No chip-select candidate provokes anything.** Asserting each of P800,
  P801, P802, P803 and P804 in turn produced no side-band movement and no
  entry in the C6's own console log. The C6 clears HANDSHAKE from a
  chip-select edge interrupt, so a live C6 on any of those nets would have
  answered without a single clock edge.

```
c6_probe: idle sideband P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P800(J26-1 in UART mux) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P801(J26-2) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P802(J26-3) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P803(J26-4 in SPI mux) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P804(J26-1 in SPI mux) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt result none
    hdr if_type=15 if_num=15 flags=0xff len=65535 offset=65535 csum=0xffff calc=0x0000 seq=65535 pkt=0xff
    frame: no esp-hosted structure
c6_probe: map HANDSHAKE=unresolved DATA_READY=unresolved
c6_probe: FAIL no esp-hosted frame mode=2 sck_hz=1000000 xfers=16 data=0 idle=0 badcsum=0
```

Note the C6 is powered independently of these signals -- it enumerates over
its own USB -- so "alive" was never evidence that the signal harness exists.

### Next step: it is a wiring question now

The remaining unknown is physical: where the C6's CS / COPI / CIPO / SCK /
HANDSHAKE / DATA_READY actually land, if they are connected at all. Firmware
cannot narrow this further by guessing pins -- the probe already drove every
Pmod1 candidate.

1. Inspect the harness between J26 and the C6 and record the real pin map. If
   the SPI leads were never run (only power and the USB console), that is the
   whole story.
2. Once the map is known, put it in `coprocessor/esp32c6/pins.env` and in the
   architecture doc, then re-run this probe unchanged: the chip-select hunt
   will name the pin and the mode sweep will resolve `HANDSHAKE=` /
   `DATA_READY=` from the C6's own behaviour.
3. Re-run with the C6 freshly reset so its queued boot INIT event is the first
   payload -- it is the richest first-light frame, and the first completed
   transaction drains it for good.

If the harness turns out to land on pins outside Pmod1, widen
`k_c6_wire_pin` in `c6_probe.h` to those candidates; the hunt logic itself
needs no change.

## Files

| File | Purpose |
|------|---------|
| `c6_probe.h` | Shared contract: tunables, protocol constants, module entry points |
| `main.c` | Bring-up, phase sequencing, side-band map and verdict |
| `src/c6_console.c` | Bounded console formatters (no newlib `printf`) |
| `src/c6_sideband.c` | Side-band sampling, wire test, chip-select hunt |
| `src/c6_frame.c` | esp-hosted payload-header decode and classification |
| `src/c6_xfer.c` | One full 1600-byte transaction; SPI mode sweep |

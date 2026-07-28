# c6_spi_probe -- first-light probe for the ESP32-C6 esp-hosted SPI link

Bring-up instrument for the raw SPI link between the RA8D2 and the ESP32-C6
wireless co-processor soldered to Pmod1 (J26). It opens the link, hand-decodes
the esp-hosted payload header out of the received bytes, and prints a single
PASS/FAIL verdict line on the board console.

It also answered two questions that nothing in this tree recorded, both of
which are now settled on the bench (see the pin map below):

1. **Which MCU pin the board's Pmod1 mux has wired to the C6 chip-select.**
   EK-RA8D2 v1 UM Table 18 p 26 makes J26-1 either `P804` (SPI position) or
   `P800` (UART position), and UM Table 3 p 16 adds SW4-3, which frees those
   pins from the on-board Octo-SPI flash only when it is ON.
2. **Which Pmod1 side-band pin carries DATA_READY and which carries
   HANDSHAKE** -- the C6 GPIO4 / GPIO6 outputs, whose RA8-side landing pins
   were never written down.

## Why `hw_pending`

`ra8_emulator` does not model an ESP32-C6 on Pmod1, so this app cannot be
gated by the EIL suite. It is also absent from the HIL suite (no `hil.conf`):
a PASS needs a C6 soldered to J26 *and* SW4-4 OFF, which takes the Arduino and
mikroBUS connectors off the board for every other app in the same run. It is a
bench instrument, run by hand.

## What it does

| Phase | What it proves |
|-------|----------------|
| Side-band read | Levels of all four Pmod1 side-band pins (P006 / P402 / P412 / P413) as no-pull inputs. No internal pull is held during the run: the C6 drives HANDSHAKE and DATA_READY push-pull, and a standing pull-up would make an unconnected pin look asserted. |
| Muxed-net wire test | Drives each of P800/P801/P802/P803/P804 high, releases it to a no-pull input and samples, then repeats driving low. A net with nothing on it holds the driven level on its own capacitance; a terminated net snaps back. |
| Chip-select hunt | Asserts each muxed-net candidate in turn and watches the side-band pins. The C6 image sets `CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y`, so a live C6 drops HANDSHAKE on the chip-select falling edge -- **no clock and no payload are needed** to find both the chip-select pin and the HANDSHAKE pin. |
| SPI mode sweep | Opens SCI2 Simple-SPI as controller at 1 MHz and clocks four full 1600-byte esp-hosted transactions per SPI mode, starting at mode 3 (what the C6 image is built with) and falling back through 0/1/2. Each frame's header is decoded and its checksum recomputed. |
| Pull-up contest | Re-reads each side-band pin with the RA8D2's internal pull-up engaged. A pin that still reads low is losing a current fight to an off-chip driver, which nothing floating can imitate. This is what identifies DATA_READY, and it runs **after** the sweep: a freshly-booted C6 holds DATA_READY high for its queued INIT event until the first transaction drains it. |

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

## Bench status: link UP, pin map confirmed (2026-07-27)

The esp-hosted SPI link is up on the bench and the RA8-side pin map is proven:

```
c6_probe: PASS esp-hosted link up mode=3 sck_hz=1000000 xfers=4 data=0 idle=4 badcsum=0
```

Mode 3, 1 MHz, four full 1600-byte transactions, **zero bad checksums**, and
the C6 answering every one of them with a well-formed idle filler frame
(`if_type=8 if_num=15 len=0`) -- the signature that proves the wire, the clock
polarity and the C6's SPI peripheral all at once. HANDSHAKE toggles exactly as
the upstream `CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y` build should: `pre=1 mid=0
post=1` on every transaction.

### Confirmed pin map

Scope-qualified end-to-end, one hole at a time (a 2 Hz 3.3 Vpp square wave
driven from the MCU and observed at the J26 hole).

| Signal | RA8D2 pin | J26 hole | C6 GPIO |
|--------|-----------|----------|---------|
| CS (Chip Select) | `P804` | J26-1 | GPIO0 |
| COPI (Controller Out) | `P801` | J26-2 | GPIO1 |
| CIPO (Controller In) | `P802` | J26-3 | GPIO2 |
| SCK (clock) | `P803` | J26-4 | GPIO3 |
| HANDSHAKE | `P006` | J26-7 | GPIO6 |
| DATA_READY | `P402` | J26-8 | GPIO4 |

`J26-9` (`P412`) and `J26-10` (`P413`) are **not connected**; J26-9 is the
hole reserved for a future host-driven EN / reset line, which is not wired
today. Ground is on J26-5 and J26-11. There is no 3V3 wire -- the C6 is
self-powered over its own USB, which is exactly why "the C6 is alive" was
never evidence that the signal harness existed.

### Required SW4 DIP positions -- this was the whole outage

| Switch | Required | Why |
|--------|----------|-----|
| SW4-1 | OFF | Pmod1 Mode Select 1 |
| SW4-2 | OFF | Pmod1 Mode Select 2; OFF+OFF selects SPI (UM Table 18 p 26) |
| SW4-3 | **ON** | Octo-SPI Inactive -- releases `P801`..`P804` to Pmod1 (UM Table 3 p 16) |
| SW4-4 | **OFF** | Arduino / mikroBUS inactive; SW4-3 ON + SW4-4 ON is not a valid combination |

The root cause of the entire outage was that this bank was **misread**: SW4-4
was ON and SW4-3 OFF. That puts the Octo-SPI flash in charge of the Pmod1 SPI
pins and holds the U6 / U9 bus switches open, so J26-1..J26-4 were never
electrically connected to the MCU at all. The board and the C6 were healthy
throughout; the four SPI signals simply terminated at an open switch.

Flipping SW4-4 OFF deactivates the Arduino and mikroBUS connectors, so the
LSM6DSO IMU Click is offline while the C6 link is in use. That is a real
trade-off on this board, not an oversight.

### How DATA_READY was identified, and why it was nearly missed

DATA_READY is the pin an edge-counting probe cannot see. It transitions
**once per boot** -- high while the C6 still holds its queued `ESPInit`
event <!-- LEGACY-OK: upstream log tag -->, low from the moment the first
completed transaction drains it -- and then sits still for the rest of the
run. That one transition falls between transactions, outside the pre / mid /
post window a per-transaction vote samples, so the vote scores it **zero
forever**.

What identifies `P402` on the bench today is that level history (high, then
low from the first transaction onward, alone among the four side-band pins)
together with the scope qualification of the J26-8 hole and the C6's own
`DR:4` configuration. The old run's own evidence line shows how invisible the
pin was to the heuristic: `P402 hs_vote=0 dr_vote=0 high=0 low=1`.

`c6_probe_pull_contest` now settles it without needing that lucky timing. It
re-reads each side-band pin as an input with the RA8D2's internal pull-up
engaged: a floating pin follows the pull and reads high, while a pin held low
by a real driver keeps reading low. Losing a pull-up fight is something no
unconnected pin can fake, and it needs no cooperation from the far end.

The contest runs *after* the transaction sweep, for the same reason the vote
misses: a freshly-booted C6 holds DATA_READY high until the first transaction
drains it, so an early contest would find nothing sunk.

**Not yet exercised on hardware.** The pin map above does not depend on it --
it was established by scope and by the level history. The next bench run of
this app is what will confirm the contest reports `P402` sunk 8/8.

## Historical record: the 2026-07-26 investigation

**The conclusions in this section were wrong.** The evidence is preserved
because it is a real measurement of a real board state -- with the SPI nets
open at the SW4 mux, every observation below is exactly what one should
expect. It is kept as a worked example of how convincing a correct
measurement can be when it is attached to a wrong premise.

At the time the conclusion drawn was "the C6 is not wired to the probed Pmod1
pins", and the switch bank had been *verified as correct*. It had not: the
positions were misread, which is what made the wiring the only remaining
suspect.

The C6 was reset mid-run while the probe sampled all four side-band pins.
Across 54 samples spanning that reset, the reading never changed:

```
### resetting C6 now (mid-run) ###
### C6 reset issued; boot log bytes: 4104 ###
=== distinct side-band readings seen across the whole run ===
     54 P006=1 P402=1 P412=0 P413=1
=== C6 confirmed to have rebooted mid-run? ===
1
```

Corroborating observations from the same runs:

- **Controller-in read all-`0xFF`** on every one of the sixteen full-size
  transactions, in all four SPI modes -- an unterminated RA8 input, not a
  peripheral pulling the line down.
- **No chip-select candidate provoked anything**, with the bus switches open:

```
c6_probe: cs-hunt P804(J26-1 in SPI mux) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt result none
c6_probe: map HANDSHAKE=unresolved DATA_READY=unresolved
c6_probe: FAIL no esp-hosted frame mode=2 sck_hz=1000000 xfers=16 data=0 idle=0 badcsum=0
```

The C6 was healthy the whole time: a clean boot of esp-hosted-mcu 2.12.11,
`Transport used :: SPI only`, `SPI Ctrl:1 mode: 3`, and the expected GPIO
assignment
(`CLK:3 MOSI:1 MISO:2 CS:0 HS:6 DR:4`, upstream's own signal names for SCK / COPI / CIPO). <!-- LEGACY-OK: quoting the co-processor's verbatim console line -->


The mis-identification the map heuristic then made is recorded in the same
run, and is what the pull-up contest exists to prevent:

```
c6_probe: evidence P006 hs_vote=5 dr_vote=0 high=1 low=1
c6_probe: evidence P402 hs_vote=0 dr_vote=0 high=0 low=1
c6_probe: evidence P412 hs_vote=0 dr_vote=0 high=1 low=0
c6_probe: evidence P413 hs_vote=1 dr_vote=1 high=1 low=1
c6_probe: map HANDSHAKE=P006 DATA_READY=P413
```

`P413` is a floating, unconnected hole. It won DATA_READY on **one** vote,
while `P402` -- held low by the C6 the whole run, `high=0` -- scored nothing
at all. A mapping now needs `k_c6_probe_min_votes` agreeing votes and an
outright win, or it is reported as `unresolved`.

## Files

| File | Purpose |
|------|---------|
| `c6_probe.h` | Shared contract: tunables, protocol constants, module entry points |
| `main.c` | Bring-up, phase sequencing, side-band map and verdict |
| `src/c6_console.c` | Bounded console formatters (no newlib `printf`) |
| `src/c6_sideband.c` | Side-band sampling, wire test, chip-select hunt |
| `src/c6_frame.c` | esp-hosted payload-header decode and classification |
| `src/c6_xfer.c` | One full 1600-byte transaction; SPI mode sweep |

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

## Bench status: BLOCKED on the SW4 DIP mux (2026-07-26)

Run on the EK-RA8D2 with the C6 flashed and idling (esp-hosted-mcu
peripheral-side FW 2.12.11). The probe works; the link does not, and the probe says why.

```
c6_probe: EK-RA8D2 <-> ESP32-C6 esp-hosted SPI probe
c6_probe: pmod1 sci2 simple-spi controller, cs on P804
c6_probe: idle sideband P006=1 P402=1 P412=0 P413=1
c6_probe: wire P800(J26-1 in UART mux) hi->1 lo->1 high-side(pull-up or driven high)
c6_probe: wire P801(J26-2) hi->1 lo->1 high-side(pull-up or driven high)
c6_probe: wire P802(J26-3) hi->1 lo->1 high-side(pull-up or driven high)
c6_probe: wire P803(J26-4 in SPI mux) hi->1 lo->1 high-side(pull-up or driven high)
c6_probe: wire P804(J26-1 in SPI mux) hi->1 lo->1 high-side(pull-up or driven high)
c6_probe: cs-hunt P800(J26-1 in UART mux) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P801(J26-2) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P802(J26-3) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P803(J26-4 in SPI mux) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt P804(J26-1 in SPI mux) no response, asserted P006=1 P402=1 P412=0 P413=1
c6_probe: cs-hunt result none
c6_probe: spi mode=3 sck_hz=1000000
  xfer 1
    pre  P006=1 P402=1 P412=0 P413=1
    mid  P006=1 P402=1 P412=0 P413=1
    post P006=1 P402=1 P412=0 P413=1
    hdr if_type=15 if_num=15 flags=0xff len=65535 offset=65535 csum=0xffff calc=0x0000 seq=65535 pkt=0xff
    frame: no esp-hosted structure
...
c6_probe: evidence P006 hs_vote=0 dr_vote=0 high=1 low=0
c6_probe: evidence P402 hs_vote=0 dr_vote=0 high=1 low=0
c6_probe: evidence P412 hs_vote=0 dr_vote=0 high=0 low=1
c6_probe: evidence P413 hs_vote=0 dr_vote=0 high=1 low=0
c6_probe: map HANDSHAKE=unresolved DATA_READY=unresolved
c6_probe: FAIL no esp-hosted frame mode=2 sck_hz=1000000 xfers=16 data=0 idle=0 badcsum=0
```

### What the run establishes

- **Every one of the sixteen full-size transactions read back all-`0xFF`**, in
  all four SPI modes. The C6's controller-in line is held with an internal
  pull-down (`gpio_set_pull_mode(GPIO_MISO, GPIO_PULLDOWN_ONLY)` in the C6's
  peripheral-side SPI driver), so a connected-but-idle C6 would read all-`0x00`.
  All-`0xFF` means the RA8's `P802` is not on the same net as the C6.
- **The chip-select hunt is the decisive measurement.** Asserting each of the
  five candidate MCU pins produced no side-band movement at all. Because the
  C6 clears HANDSHAKE from a chip-select edge interrupt, a live C6 on any of
  those nets would have answered without a single clock edge. None did.
- **The side-band pins never moved** across any phase of the run
  (`hs_vote=0 dr_vote=0` for all four; each pin stayed at one level).

Taken together: **the Pmod1 SPI signal group is not electrically reaching J26.**
That is a board-mux fact, not a firmware fact. Per EK-RA8D2 v1 UM Rev 1.01
Table 3 p 16 and Table 18 p 26 the required DIP positions are

| Switch | Required | Meaning |
|--------|----------|---------|
| SW4-1 | **OFF** | Pmod1 Mode Select 1 |
| SW4-2 | **OFF** | Pmod1 Mode Select 2; OFF+OFF selects **SPI** (ON/OFF is UART, OFF/ON is I2C) |
| SW4-3 | **ON** | Octo-SPI **Inactive** -- this is what frees P801..P804 for Pmod1 |

Note the trap in the UART position: J26-1 becomes `P800` and J26-4 becomes
`P804`, so a controller driving `P804` as chip-select is really feeding the
C6's *clock* pin and the C6 never sees a chip-select at all -- exactly the
silent failure this probe was written to catch.

The U15 PI4IOE5V6408 expander (I2C 0x43) can override SW4 in software, but a
prior whole-output-space sweep (issue #44, recorded on
`ra8_board_io_expander_set_octospi_active`) established that its GPIOs are
**not** in the Octo-SPI path and that SW4-3 is a hardware-only analog mux. The
switch has to be set on the board.

**SW4-3 is the prime suspect**, because SW4-1 and SW4-2 are already OFF/OFF in
the UM's default configuration -- that is the SPI position -- while SW4-3's
default is OFF, which is *Octo-SPI Active* and which UM Table 3 p 16 lists as
conflicting with "Arduino, mikroBUS and Pmod 1 (SPI, UART)". So the one switch
that must be moved off its factory default for this link to work is SW4-3.

### The one alternative this run cannot exclude

"No chip-select response" is also what a C6 whose esp-hosted application had
wedged would look like, since the HANDSHAKE drop comes from a GPIO interrupt
inside that application. The evidence against it is circumstantial but strong:
the C6 enumerates its USB Serial/JTAG interface (so the part is powered and
running), it had been idling untouched since a clean flash with a confirmed
boot log, and an idle esp-hosted peripheral is only ever blocked on queues.

Discriminate in this order:

1. Set the three switches above, power-cycle, re-run the probe. If the
   chip-select hunt now names a pin, the mux was the whole story.
2. If it still reports `none`, read the C6 console to check that esp-hosted is
   alive before suspecting the wiring itself.

### Next step

Set SW4-1 OFF / SW4-2 OFF / SW4-3 ON on the EVM and re-run. The probe needs no
change: the chip-select hunt will name the pin that reaches the C6 and the mode
sweep will resolve `HANDSHAKE=` / `DATA_READY=` from the C6's own behaviour.

Re-running is worth a power cycle first (`bash scripts/hil/tapo.sh board cycle`)
so the C6 has a freshly queued boot INIT event on `ESP_PRIV_IF`: that event is
the richest first-light payload, and the first completed transaction drains it
for good.

## Files

| File | Purpose |
|------|---------|
| `c6_probe.h` | Shared contract: tunables, protocol constants, module entry points |
| `main.c` | Bring-up, phase sequencing, side-band map and verdict |
| `src/c6_console.c` | Bounded console formatters (no newlib `printf`) |
| `src/c6_sideband.c` | Side-band sampling, wire test, chip-select hunt |
| `src/c6_frame.c` | esp-hosted payload-header decode and classification |
| `src/c6_xfer.c` | One full 1600-byte transaction; SPI mode sweep |

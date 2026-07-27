# c6_hosted_init -- bring the esp-hosted RA8 port up against the ESP32-C6

First application to consume `port/esp-hosted/`, the first-party RA8D2 +
ThreadX port of the vendored esp-hosted host driver. It is the port's
buildable consumer and its bring-up harness: it initialises the port, states
the pin map and interrupt routing the port resolved, reads both side-band
lines through the OS-abstraction vtable, clocks exactly one full-duplex
transaction, decodes the frame that came back and prints a single PASS/FAIL
verdict line on the board console.

## What it proves, and what it does not

**It proves** that the port compiles and links into a real firmware image,
that `ra8_esp_hosted_port_init()` runs to completion under ThreadX, that the
pin map and interrupt routing resolve to the nets the board layer names, and
that the vendored driver's `g_h.funcs` vtable is populated well enough to
sample a GPIO and start a transaction.

**It does not prove that the link works.** No hardware run has ever happened.
The application has never completed a transaction against a real ESP32-C6,
because the C6 is not currently connected to the RA8 pins:

> The landed probe `examples/ek_ra8d2/hw_pending/c6_spi_probe` reset the C6
> *in the middle* of a run while sampling all four Pmod1 side-band pins.
> Across 54 samples spanning that reset the reading never changed once, and
> asserting each of the five muxed chip-select candidates provoked no
> response on the co-processor's own console. The SW4 mux is correct and the
> C6 is alive and armed; the signal harness between them is what is missing.

The owner is rebuilding that harness. Until it exists, this application's
verdict line is expected to read FAIL, and the reason it names is the honest
one -- see "What a FAIL looks like today" below.

## Why `hw_pending`

`ra8_emulator` does not model an ESP32-C6 on Pmod1, so this app cannot be
gated by the SIL suite, and it is deliberately absent from the HIL suite (no
`hil.conf`) while the bench blocker above stands. It is a bench instrument,
run by hand, and it stays under `hw_pending/` until a run on real hardware
promotes it.

## Pin map

### C6 side -- settled and bench-proven

From `coprocessor/esp32c6/pins.env`, which is the single source of truth for
the co-processor image (esp-hosted-mcu `949bb30`, firmware `2.12.11`, ESP-IDF
`v5.5.4`). The inclusive name is ours; the parenthesised one is Espressif's.

| Signal | C6 GPIO |
|---|---|
| CS (Chip Select) | GPIO0 |
| COPI (Controller Out / MOSI) | GPIO1 | <!-- LEGACY-OK: naming the Espressif signal name a reader will meet in pins.env -->
| CIPO (Controller In / MISO) | GPIO2 | <!-- LEGACY-OK: naming the Espressif signal name a reader will meet in pins.env -->
| SCK (clock / CLK) | GPIO3 |
| DATA_READY (co-processor -> host) | GPIO4 |
| HANDSHAKE (co-processor -> host) | GPIO6 |
| RESET | disconnected (-1) |

The C6's own boot banner confirms this assignment verbatim
(`CLK:3 MOSI:1 MISO:2 CS:0 HS:6 DR:4` -- upstream's own signal names for SCK / COPI / CIPO), <!-- LEGACY-OK: quoting the co-processor's verbatim console line -->
together with `Transport used :: SPI only` and `SPI Ctrl:1 mode: 3`.

### RA8 side -- provisional until the harness is characterised

`port/esp-hosted/inc/ra8_esp_hosted_pins.h` is **the one file to edit** when
the rebuilt harness is measured. Every other consumer derives from it: the
`H_GPIO_*` macros in `port_esp_hosted_host_config.h`, the interrupt-routing
table in `ra8_esp_hosted_pins.c`, and this application, which prints whatever
that header resolves to rather than restating any of it. No EK-RA8D2 pin
number is written anywhere in this app's sources.

| Signal | Board-layer enumerator | Pmod1 net today |
|---|---|---|
| Chip select | `k_ra8_board_pmod1_spi_cs` | Pmod1.1 (SPI mux position) |
| COPI | `k_ra8_board_pmod1_spi_copi` | Pmod1.2 |
| CIPO | `k_ra8_board_pmod1_spi_cipo` | Pmod1.3 |
| SCK | `k_ra8_board_pmod1_spi_sck` | Pmod1.4 |
| HANDSHAKE | `k_ra8_board_pmod1_gpio_a` | Pmod1.9 |
| DATA_READY | `k_ra8_board_pmod1_irq` | Pmod1.7 |
| RESET | `k_ra8_pin_none` | not wired (matches the C6 side) |

Pmod1 SPI needs SW4-1 OFF, SW4-2 OFF (Pmod1 SPI position) and SW4-3 ON
(Octo-SPI inactive, so the mux frees those pins). Both were confirmed on the
board during the probe run.

### Side-band interrupt routing: one ICU edge, one polled pin

The ICU external-interrupt inputs are concentrated on port 0 in this package,
so of the four Pmod1 side-band nets **only Pmod1.7 has an IRQ channel**
(IRQ11, deep-standby capable). The port therefore does not assume every
side-band pin can raise an edge:

| Signal | Path | Why |
|---|---|---|
| DATA_READY | ICU edge, channel 11 | Sits on the one Pmod1 side-band net with a channel, and is the latency-sensitive line -- it is what tells the host a frame is waiting. |
| HANDSHAKE | Software edge detector, 2 ms poll | Its net has no ICU channel. The port samples it on a bounded ThreadX timer and raises the same callback, so the vendored driver never sees the difference. |

`ra8_esp_hosted_pin_irq_num()` is a pure table lookup, and this application
calls it and prints the answer, so the console states the routing that will
really be used rather than what the package is assumed to offer. If the
rebuilt harness lands DATA_READY somewhere with no channel, the printed line
changes to `path=software-edge-detector` on its own.

## Link parameters

| Parameter | Value | Source |
|---|---|---|
| SPI mode | 3 (CPOL 1 / CPHA 1) | `CONFIG_ESP_SPI_MODE=3` in the C6 image |
| Bit rate | 5 MHz | `H_SPI_FD_CLK_MHZ`; upstream's recommended evaluation clock, an order of magnitude above the probe's deliberately slow 1 MHz |
| Frame | 1600 bytes, full duplex | `ESP_TRANSPORT_SPI_MAX_BUF_SIZE` |
| Checksum | on | `CONFIG_ESP_SPI_CHECKSUM=y` |
| Bus | SCI2 Simple-SPI, controller | `k_ra8_board_pmod1_sci_channel` |

## Build and run

```sh
make c6_hosted_init                      # from the repo root
bash scripts/hil/flash.sh c6_hosted_init # program + release from reset
```

Console: J-Link OB VCOM, 115200 8N1.

`ra8_add_app()` compiles `main.c` plus every `.c` under `src/`, so the file
split below needs no CMake change.

| File | Purpose |
|---|---|
| `c6_hosted.h` | Shared contract: application-owned parameters, formatter bounds, module entry points |
| `main.c` | Bring-up sequence, ThreadX entry, worker thread, heartbeat |
| `src/c6_hosted_console.c` | Bounded console formatters and the two pin printers (no newlib `printf`) |
| `src/c6_hosted_report.c` | Banner, resolved pin map with interrupt paths, side-band sampling, event handler |
| `src/c6_hosted_frame.c` | Idle-frame build, the single transaction, header decode and verdict |

## Where ThreadX object creation happens

`ra8_esp_hosted_port_init()` creates ThreadX byte pools, mutexes and timers,
so it must run where ThreadX permits object creation. `main()` therefore does
only the bare-metal half -- clocks, module-stop, SysTick, console, banner --
and then calls `tx_kernel_enter()`. The port init runs from
`tx_application_define()`, which also registers the event handler *before* the
init and starts the single worker thread. The worker is the first context that
can print after the init, so it reports the exact `ra8_err_t` the init
returned.

## What a PASS looks like

PASS requires **all** of the following:

1. `g_h.funcs->_h_do_bus_transfer()` returned `RET_OK`.
2. The 1600-byte receive buffer is neither uniformly `0x00` nor uniformly
   `0xFF`.
3. The received header's `offset` equals `sizeof(struct esp_payload_header)`
   (12).
4. The received `len` is within `MAX_PAYLOAD_SIZE` (1588).
5. The received checksum equals the one recomputed over the frame with the
   checksum field taken as zero.

```
c6_hosted_init: port_init=k_ra8_ok
c6_hosted_init: sideband handshake level=1 active_level=1 state=asserted
c6_hosted_init: sideband data_ready level=1 active_level=1 state=asserted
c6_hosted_init: transfer rc=0 frame_bytes=1600
c6_hosted_init: rx if_type=... len=... offset=12 seq_num=... csum=0x1234 calc=0x1234
c6_hosted_init: PASS link up
```

The freshest first-light frame is the C6's queued boot INIT event, and the
first completed transaction drains it for good -- so reset the C6 immediately
before a run if you want to see it.

## What a FAIL looks like today

With the harness disconnected, the co-processor never drives CIPO. The RA8
input is unterminated, so the whole receive buffer reads `0xFF`, and the
verdict names that directly rather than reporting a checksum mismatch -- a
missing wire and a mis-clocked link are different faults:

```
c6_hosted_init: transfer rc=0 frame_bytes=1600
c6_hosted_init: rx if_type=15 if_num=15 flags=0xff len=65535 offset=65535 seq_num=65535 csum=0xffff calc=0x0000
c6_hosted_init: FAIL receive buffer all-ones -- co-processor did not drive the bus
```

A *connected* but idle C6 would instead read all-zero, because it holds its
controller-in line with an internal pull-down; that case is reported as
`receive buffer all-zero -- co-processor did not drive the bus`. The
distinction is exactly what tells you whether the wire exists.

The heartbeat loop keeps running afterwards, re-reading both side-band lines
every two seconds, so a bench with no scope can still see the moment the C6
first raises a line:

```
c6_hosted_init: heartbeat n=7 events=0
c6_hosted_init: sideband handshake level=0 active_level=1 state=deasserted
c6_hosted_init: sideband data_ready level=1 active_level=1 state=asserted
```

If `ra8_esp_hosted_port_init()` itself fails, the worker prints the exact
`ra8_err_t` and attempts **no** transaction -- a port that did not come up has
an unpopulated vtable, and calling through it would fault rather than report.

## Next step: it is a wiring question

Firmware cannot narrow this further by guessing pins; `c6_spi_probe` already
drove every Pmod1 candidate. The remaining unknown is physical.

1. Inspect the harness between J26 and the C6 and record the real pin map.
2. Put it in `coprocessor/esp32c6/pins.env` (C6 side) and in
   `port/esp-hosted/inc/ra8_esp_hosted_pins.h` (RA8 side). If a side-band pin
   lands somewhere with an ICU channel, add its row to
   `k_ra8_esp_hosted_irq_map` in `ra8_esp_hosted_pins.c` too.
3. Re-run this application unchanged. The banner will state the new map and
   the new interrupt paths, and the verdict line will say whether the link
   came up.

## See also

- `docs/SOUP/esp-hosted-host.md` -- the vendored host driver: what is
  compiled, what is excluded and why, and the pinned upstream commit.
- `docs/design/c6_wireless_architecture.md` -- how the co-processor fits into
  the system.
- `examples/ek_ra8d2/hw_pending/c6_spi_probe/README.md` -- the raw-SPI probe
  that established the bench blocker, with the full measurement transcript.

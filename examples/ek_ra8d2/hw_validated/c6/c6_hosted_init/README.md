# c6_hosted_init -- bring the esp-hosted RA8 port up against the ESP32-C6

First application to consume `port/esp-hosted/`, the first-party RA8D2 +
ThreadX port of the vendored esp-hosted host driver. It is the port's
buildable consumer and its bring-up harness: it initialises the port, states
the pin map and interrupt routing the port resolved, reads both side-band
lines through the OS-abstraction vtable, clocks exactly one full-duplex
transaction, decodes the frame that came back and prints a single PASS/FAIL
verdict line on the board console.

## What it proves, and what it does not

**Established with no board attached.** The port compiles and links into a
real firmware image, the pin map and interrupt routing resolve to nets the
board layer names, and the port's own checks are covered by host tests. The
cross-build gate carries all of that on every push.

**Established on the bench -- but by a different program.** The raw wire is
proven. On 2026-07-27 `examples/ek_ra8d2/hw_validated/c6/c6_spi_probe`
scope-qualified every J26 hole end to end, then brought the link up at SPI
mode 3 / 1 MHz with zero bad checksums. That run is what fixed the pin map
recorded below, and it retired the 2026-07-26 "no harness" diagnosis this
README used to carry.

**Established on the bench by this application, 2026-07-28.** The port runs on
silicon. `ra8_esp_hosted_port_init()` returns `k_ra8_ok` under ThreadX, the
vendored `g_h.funcs` vtable is populated well enough to sample both side-band
GPIOs, and a **5 MHz** transaction lands a decodable frame -- five times the
rate the probe qualified, which was the one link parameter the 1 MHz proof did
not cover. The capture is below, and `make hil-c6` re-runs it.

## Why the `c6` tier and not `hw_validated/hil/`

`ra8_emulator` models no ESP32-C6 (#494), so this app cannot be gated by the
EIL suite -- and `check_hil_eil_parity.py` rightly requires every app under
`hw_validated/hil/` to be. It also needs SW4-4 OFF, which takes the Arduino and
mikroBUS connectors off the board for every other app in the same pass. Both
reasons put it in `examples/ek_ra8d2/hw_validated/c6/`, whose
[README](../README.md) states the bench configuration; it is `hw_validated`
because it passes on hardware, and separate because the bench cannot serve both
tiers in one run.

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

### RA8 side -- settled and bench-proven 2026-07-27

`port/esp-hosted/inc/ra8_esp_hosted_pins.h` is **the one file to edit** if the
harness ever moves again. Every other consumer derives from it: the `H_GPIO_*`
macros in `port_esp_hosted_host_config.h`, the interrupt-routing table in
`ra8_esp_hosted_pins.c`, and this application, which prints whatever that
header resolves to rather than restating any of it. No EK-RA8D2 pin number is
written anywhere in this app's sources.
`scripts/checks/check_c6_pin_config.py` diffs that header against
`coprocessor/esp32c6/pins.env` on every CI run, so the C code and the
co-processor image cannot drift apart.

| Signal | Board-layer enumerator | Pmod1 net | J26 | RA8D2 pin |
|---|---|---|---|---|
| Chip select | `k_ra8_board_pmod1_spi_cs` | Pmod1.1 (SPI mux position) | J26-1 | `P804` |
| COPI | `k_ra8_board_pmod1_spi_copi` | Pmod1.2 | J26-2 | `P801` |
| CIPO | `k_ra8_board_pmod1_spi_cipo` | Pmod1.3 | J26-3 | `P802` |
| SCK | `k_ra8_board_pmod1_spi_sck` | Pmod1.4 | J26-4 | `P803` |
| HANDSHAKE | `k_ra8_board_pmod1_irq` | Pmod1.7 | J26-7 | `P006` |
| DATA_READY | `k_ra8_board_pmod1_reset` | Pmod1.8 | J26-8 | `P402` |
| RESET | `k_ra8_pin_none` | not wired | -- | -- |

The board-layer enumerators are named for the Pmod connector's nominal pin
*roles*, not for this link's signals: `_irq` is simply the Pmod1.7 net and
`_reset` the Pmod1.8 net. What the C6 drives on each is the table above, and
RESET genuinely has no RA8-side landing pin -- matching `C6_PIN_RESET=-1`, the
co-processor is reset by power-cycling its own USB. J26-9 (`P412`) is the hole
reserved for a future host-driven EN line.

Pmod1 SPI needs SW4-1 OFF, SW4-2 OFF (Pmod1 SPI position), SW4-3 **ON**
(Octo-SPI inactive, so the mux frees `P801`..`P804`) and SW4-4 **OFF**
(Arduino / mikroBUS inactive; SW4-3 ON together with SW4-4 ON is invalid).
Getting SW4-3 wrong leaves J26-1..J26-4 electrically disconnected from the MCU
while the board and the C6 both look perfectly healthy -- that misreading, not
a wiring fault, was the whole 2026-07-26 outage.

### Side-band interrupt routing: one ICU edge, one polled pin

The ICU external-interrupt inputs are concentrated on port 0 in this package,
so of the four Pmod1 side-band nets **only Pmod1.7 (`P006`) has an IRQ
channel** (IRQ11, deep-standby capable). The port therefore does not assume
every side-band pin can raise an edge:

| Signal | Path | Why |
|---|---|---|
| HANDSHAKE | ICU edge, channel 11 | It lands on the one Pmod1 side-band net with a channel, and it is the signal that needs an edge: the C6 image sets `CONFIG_ESP_SPI_DEASSERT_HS_ON_CS`, so HANDSHAKE pulses for the length of a chip-select assertion and a poll can miss the pulse outright. |
| DATA_READY | Software edge detector, 2 ms poll | Its net (`P402`) has no ICU channel, and losing an edge there costs nothing: the C6 holds DATA_READY asserted until the host drains the queued frame, so a poll can be late but cannot miss the condition. The port samples it on a bounded ThreadX timer and raises the same callback, so the vendored driver never sees the difference. |

`ra8_esp_hosted_pin_irq_num()` is a pure table lookup, and this application
calls it and prints the answer, so the console states the routing that will
really be used rather than what the package is assumed to offer. If a future
harness moves a side-band net, the printed line follows on its own.

## Link parameters

| Parameter | Value | Source |
|---|---|---|
| SPI mode | 3 (CPOL 1 / CPHA 1) | `CONFIG_ESP_SPI_MODE=3` in the C6 image |
| Bit rate | 5 MHz | `H_SPI_FD_CLK_MHZ`; upstream's recommended evaluation clock. The bench proof was taken at the probe's deliberately slow 1 MHz, so this five-fold step-up is itself unverified and is a candidate cause if the first run reports a checksum mismatch |
| Frame | 1600 bytes, full duplex | `ESP_TRANSPORT_SPI_MAX_BUF_SIZE` |
| Checksum | on | `CONFIG_ESP_SPI_CHECKSUM=y` |
| Bus | SCI2 Simple-SPI, controller | `k_ra8_board_pmod1_sci_channel` |

## Build and run

```sh
make c6_hosted_init                      # from the repo root
make hil-c6 APP=c6_hosted_init           # build, flash and verify on the bench
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

## Observed output (2026-07-28)

PASS requires **all** of the following:

1. `g_h.funcs->_h_do_bus_transfer()` returned `RET_OK`.
2. The 1600-byte receive buffer is neither uniformly `0x00` nor uniformly
   `0xFF`.
3. Either the frame is the co-processor's **idle filler** -- `if_type =
   ESP_MAX_IF (8)`, `if_num = 0x0F`, zero length -- or every remaining test
   below holds.
4. The received header's `offset` equals `sizeof(struct esp_payload_header)`
   (12).
5. The received `len` is within `MAX_PAYLOAD_SIZE` (1588).
6. The received checksum equals the one recomputed over the frame with the
   checksum field taken as zero.

Criterion 3 is not a relaxation, it is the fix for a false negative this
application shipped with. A filler frame legitimately carries `offset = 0`,
because it has no payload to point at; the first silicon run judged one by the
rules for a data frame and printed `FAIL header offset is not the
payload-header size` at a link that was working perfectly. `c6_spi_probe`, which
had actually seen a filler frame, classified it correctly all along.

```
c6_hosted_init: EK-RA8D2 <-> ESP32-C6 esp-hosted port bring-up
c6_hosted_init: cpuclk0_hz=1000000000 pclka_hz=125000000
c6_hosted_init: spi sci=2 mode=3 sck_hz=5000000
c6_hosted_init: bus cs=port8.pin4 copi=port8.pin1 cipo=port8.pin2 sck=port8.pin3
c6_hosted_init: driver-view handshake=port0.pin6 data_ready=port4.pin2 reset=unwired
c6_hosted_init: handshake=port0.pin6 irq=11 path=icu-edge
c6_hosted_init: data_ready=port4.pin2 irq=none path=software-edge-detector poll_ms=2
c6_hosted_init: port_init=ok
c6_hosted_init: sideband handshake level=1 active_level=1 state=asserted
c6_hosted_init: sideband data_ready level=0 active_level=1 state=deasserted
c6_hosted_init: transfer rc=0 frame_bytes=1600
c6_hosted_init: rx if_type=8 if_num=15 flags=0x00 len=0 offset=0 seq_num=0 csum=0x0000 calc=0x00f8
c6_hosted_init: PASS link up -- co-processor returned its idle filler frame
```

The two routing lines are the cheapest confirmation that the app is running
against the map above: `port0.pin6` is `P006` (HANDSHAKE, on the ICU channel)
and `port4.pin2` is `P402` (DATA_READY, polled). They are printed from the
same lookup the port itself uses, so they cannot disagree with it.

A freshly-booted C6 reads `data_ready state=asserted` because it is still
holding its queued boot INIT event, and the first completed transaction drains
that event for good. If you want the INIT frame rather than the idle filler,
power-cycle the C6 immediately before the run.

### What a FAIL means now

A FAIL no longer means "no harness", and no longer means "the port is
untested": both the wire and the port are proven. A failing verdict now points
at the bench setup first, and at a regression in the port second:

| Verdict line | What it most likely means |
|---|---|
| `FAIL port init failed -- no transaction attempted` | `ra8_esp_hosted_port_init()` returned non-`k_ra8_ok`; the exact code is on the preceding `port_init=` line. No transaction is attempted, because a port that did not come up has an unpopulated vtable and calling through it would fault rather than report. |
| `FAIL bus transfer did not return RET_OK` | The port's `_h_do_bus_transfer` implementation or the SCI2 Simple-SPI bus beneath it failed. Entirely first-party ground. |
| `FAIL receive buffer all-zero -- co-processor did not drive the bus` | The C6 holds its controller-in line with an internal pull-down, so all-zero is what a *connected* co-processor that never saw a valid transaction looks like: chip select never asserted for the frame, or clocked in a mode the C6 rejects. |
| `FAIL receive buffer all-ones -- co-processor did not drive the bus` | The RA8 input floated. With the harness proven this points at the bench rather than the code: SW4-3 back OFF, the C6 unpowered, or a lifted joint. Re-run `c6_spi_probe` to separate wire from firmware. |
| `FAIL header offset is not the payload-header size`, `FAIL advertised length exceeds MAX_PAYLOAD_SIZE`, `FAIL checksum mismatch` | The bus is alive and the C6 is driving -- framing or timing is wrong. 5 MHz is no longer a suspect: it is the rate every run since 2026-07-28 has used, with zero bad checksums. Re-run `c6_spi_probe` to separate wire from firmware. |

```
c6_hosted_init: transfer rc=0 frame_bytes=1600
c6_hosted_init: rx if_type=0 if_num=0 flags=0x00 len=0 offset=12 seq_num=3 csum=0x1234 calc=0x8a01
c6_hosted_init: FAIL checksum mismatch
```

The heartbeat loop keeps running after either verdict, re-reading both
side-band lines every two seconds, so a bench with no scope can still watch
the C6 raise and drop a line:

```
c6_hosted_init: heartbeat n=7 events=0
c6_hosted_init: sideband handshake level=1 active_level=1 state=asserted
c6_hosted_init: sideband data_ready level=0 active_level=1 state=deasserted
```

That pairing -- HANDSHAKE asserted, DATA_READY deasserted -- is the steady
state of a healthy link with nothing queued, and is worth seeing even on a run
whose single transaction failed.

## Next step: the protocol

The wire and the port are both settled. What this application does not touch is
the protocol above them -- it clocks one transaction and reads the header, and
never asks the co-processor for anything. That is
[`../c6_fw_version/`](../c6_fw_version/README.md): a real RPC request, parsed
by the co-processor, answered with a populated response whose fields are
checked.

To re-run this one:

1. Set SW4 to 1=OFF, 2=OFF, 3=ON, 4=OFF and power the C6 over its own USB.
2. `make hil-c6 APP=c6_hosted_init`.
3. Compare the two routing lines against the pin map above, then read the
   verdict. On a FAIL, work down the table above, starting with
   `make hil-c6 APP=c6_spi_probe` to separate wire from firmware.

## See also

- `docs/SOUP/esp-hosted-host.md` -- the vendored host driver: what is
  compiled, what is excluded and why, and the pinned upstream commit.
- `docs/design/c6_wireless_architecture.md` -- how the co-processor fits into
  the system.
- `examples/ek_ra8d2/hw_validated/c6/c6_spi_probe/README.md` -- the raw-SPI probe
  that established the pin map above, with the full measurement transcript
  including the superseded 2026-07-26 diagnosis.

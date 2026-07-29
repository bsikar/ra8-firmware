# examples/ek_ra8d2/hw_validated/c6/

Apps that talk to the **ESP32-C6 companion radio** over esp-hosted. Every one
of them has been run on real silicon and passes; they are `hw_validated`, not
`hw_pending`. They live in their own tier because of the bench, not because of
their maturity.

Run the whole tier with:

```sh
make hil-c6              # all four, in order
make hil-c6 APP=c6_spi_probe
```

That is `scripts/hil/all.sh --dir examples/ek_ra8d2/hw_validated/c6` -- the
same runner, the same per-app `hil.conf` manifests and the same bench hold the
default suite uses. There is one HIL runner in this tree, not one per bench
configuration.

## Why they are not in `hw_validated/hil/`

Two independent reasons, and either alone would be enough.

**The DIP switches are mutually exclusive with the default pass.** These apps
need **SW4 1=OFF 2=OFF 3=ON 4=OFF**. SW4-3 ON deactivates Octo-SPI so the U6/U9
bus switches connect `P801`..`P804` to J26 at all; SW4-4 OFF deactivates the
Arduino and mikroBUS connectors, which the rest of the tree's apps need. One
run of the bench cannot satisfy both settings, so these cannot share a pass
with `make hil-all`.

> Getting SW4-3 wrong leaves J26-1..J26-4 electrically disconnected from the
> MCU while the board and the co-processor both look perfectly healthy. That
> misreading -- not a wiring fault -- cost the whole 2026-07-26 bench day. If
> the link stops working, check the switch bank **electrically** before
> suspecting anything else.

**`ra8_emulator` models no ESP32-C6** (#494). `hw_validated/hil/` is bound by
`check_hil_eil_parity.py` to the EIL suite: every app there must also be
exercised in the emulator, with no skips. That gate is right, and these apps
genuinely cannot satisfy it yet, so putting them there would mean either a
failing gate or a hole punched in one. They stay outside it until the emulator
grows a co-processor model.

## The apps, in the order to run them when triaging

| App | What it proves | Verdict line |
|---|---|---|
| `c6_spi_probe` | The physical link. Drives SCI2 Simple-SPI directly -- no port, no vendored driver -- characterising every J26 hole, hunting for the pin the co-processor answers chip-select on, then clocking esp-hosted transactions at a deliberately slow 1 MHz. | `c6_probe: PASS esp-hosted link up` |
| `c6_hosted_init` | `port/esp-hosted/` on silicon. Brings the RA8D2 + ThreadX port up, prints the pin map and interrupt routing it resolved, and clocks one full-duplex transaction at 5 MHz. | `c6_hosted_init: PASS link up` |
| `c6_fw_version` | The **protocol**. A real RPC request goes up, the co-processor parses it, and a populated response comes back whose fields are checked. Built by hand inside the app. | `c6_fwver: PASS esp-hosted RPC round-trip` |
| `c6_wifi_link` | The **facade** (`libs/ra8_c6link`), and the co-processor's acceptance of a real `Req_WifiInit` configuration -- the one part of the control plane no host test can settle. Takes the station up, reads its MAC, tears it down. | `c6_wifi: PASS ra8_c6link drove the coprocessor station up` |

They form a ladder: when the top one fails, the one below separates "the wire
is wrong" from "the firmware is wrong". `c6_spi_probe` is the bench's negative
control and is the first thing to run, always.

`c6_fw_version` and `c6_wifi_link` overlap deliberately: the former builds the
protocol by hand and the latter goes through `libs/ra8_c6link`, so if one passes
and the other fails, the difference is the facade rather than the link.

## Bench setup

The harness is stripped 22AWG jumpers between J26 and the C6 dev board's
headers. There is **no 3V3 wire**: the C6 is powered from its own USB, and
J26-6 / J26-12 must stay empty.

| Signal | RA8D2 pin | J26 | C6 GPIO |
|---|---|---|---|
| CS (Chip Select) | `P804` | J26-1 | GPIO0 |
| COPI | `P801` | J26-2 | GPIO1 |
| CIPO | `P802` | J26-3 | GPIO2 |
| SCK | `P803` | J26-4 | GPIO3 |
| HANDSHAKE | `P006` | J26-7 | GPIO6 |
| DATA_READY | `P402` | J26-8 | GPIO4 |
| GND | -- | J26-5, J26-11 | GND |

`port/esp-hosted/inc/ra8_esp_hosted_pins.h` is the one file to edit if the
harness moves; `coprocessor/esp32c6/pins.env` is the one file the co-processor
image is built from, and `scripts/checks/check_c6_pin_config.py` diffs them on
every CI run so they cannot drift apart.

The co-processor runs esp-hosted-mcu peripheral-side firmware **2.12.11** (ESP-IDF
v5.5.4, esp-hosted-mcu `949bb30`). Do not reflash it without a specific reason:
`c6_fw_version` asserts that version against the vendored host driver's own, so
a co-processor reflash without a matching vendor bump turns that test red on
purpose.

## Two bench facts that repeatedly cost time

**`/dev/ttyACM*` numbering swaps across power cycles.** The J-Link, the chip's
own USBHS CDC and the C6's CH343 bridge all enumerate in that namespace. Always
address consoles through `/dev/serial/by-id/`; the HIL scripts already do
(`scripts/hil/lib/tty_resolve.sh`).

**The co-processor's boot announcements are one-shot.** It queues them at boot
and holds them until a transaction drains them, so whatever image is already in
MRAM will eat them the moment the board powers up. To see them, flash a
neutral app first, then power-cycle, then flash the app you want:

```sh
bash scripts/hil/flash.sh blink
make hil-tapo TARGET=board CMD=cycle
make hil-c6 APP=c6_fw_version
```

## See also

- `docs/design/c6_wireless_architecture.md` -- how the co-processor fits into
  the system.
- `docs/SOUP/esp-hosted-host.md` -- the vendored host driver: what is compiled,
  what is excluded and why.
- `coprocessor/esp32c6/README.md` -- building and flashing the co-processor image.

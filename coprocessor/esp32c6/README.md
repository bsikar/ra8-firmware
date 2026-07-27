# ESP32-C6 wireless co-processor firmware

The EK-RA8D2 gains Wi-Fi + Bluetooth by pairing the RA8D2 host with an
**ESP32-C6** acting as a wireless co-processor. The C6 runs Espressif's
**esp-hosted-mcu** `network_adapter` application (the peripheral-side, or
upstream "slave", firmware) over a SPI transport; the RA8 is the host that <!-- LEGACY-OK: names the upstream esp-hosted-mcu role verbatim; our term is peripheral-side -->
drives it.

**Zero first-party code runs on the C6.** The C6 image is entirely Espressif
esp-hosted-mcu, vendored-in as Software Of Unknown Provenance (SOUP). This
directory holds only the *build recipe* (pinned versions, our proven
`sdkconfig.defaults`, and reproducible build/flash scripts) -- not any source
that runs on the C6. The RA8-side host driver is a separate follow-on and is
**not** part of this directory.

- SOUP qualification: [`../../docs/SOUP/esp-hosted.md`](../../docs/SOUP/esp-hosted.md)
- Architecture: [`../../docs/design/c6_wireless_architecture.md`](../../docs/design/c6_wireless_architecture.md)

## Pinned versions

Single source of truth: [`pins.env`](pins.env).

| Item | Value |
|------|-------|
| esp-idf | `v5.5.4` |
| esp-hosted-mcu | commit `949bb30` (`949bb30612747a3bd9e402eda8d01fbfa1f8503e`) |
| esp-hosted-mcu FW | `2.12.11` (`network_adapter` app) |
| Target | `esp32c6` |

## SPI pin map (ESP32-C6 GPIO numbers)

Inclusive name first, Espressif legacy name in parentheses.

| Signal | GPIO | esp-idf `sdkconfig` key |
|--------|------|-------------------------|
| CS (Chip Select) | GPIO0 | `CONFIG_ESP_SPI_HSPI_GPIO_CS` |
| COPI (Controller Out / MOSI) | GPIO1 | `CONFIG_ESP_SPI_HSPI_GPIO_MOSI` | <!-- LEGACY-OK: esp-idf sdkconfig key and legacy signal name; our signal is COPI -->
| CIPO (Controller In / MISO) | GPIO2 | `CONFIG_ESP_SPI_HSPI_GPIO_MISO` | <!-- LEGACY-OK: esp-idf sdkconfig key and legacy signal name; our signal is CIPO -->
| SCK (clock / CLK) | GPIO3 | `CONFIG_ESP_SPI_HSPI_GPIO_CLK` |
| DATA_READY | GPIO4 | `CONFIG_ESP_SPI_GPIO_DATA_READY` |
| HANDSHAKE | GPIO6 | `CONFIG_ESP_SPI_GPIO_HANDSHAKE` |
| RESET | disconnected (-1) | `CONFIG_ESP_SPI_GPIO_RESET` |

## RA8-side landing pins (EK-RA8D2 Pmod1 / J26) -- bench-proven 2026-07-27

`pins.env` records both ends of every wire (`RA8_PIN_*` / `RA8_J26_*`); a pin
map that names only the C6 GPIO does not tell anyone which MCU pin to read.

| Signal | RA8D2 pin | J26 hole | C6 GPIO |
|--------|-----------|----------|---------|
| CS (Chip Select) | `P804` | J26-1 | GPIO0 |
| COPI (Controller Out) | `P801` | J26-2 | GPIO1 |
| CIPO (Controller In) | `P802` | J26-3 | GPIO2 |
| SCK (clock) | `P803` | J26-4 | GPIO3 |
| HANDSHAKE | `P006` | J26-7 | GPIO6 |
| DATA_READY | `P402` | J26-8 | GPIO4 |

J26-5 / J26-11 are ground. J26-9 (`P412`) and J26-10 (`P413`) are unconnected;
J26-9 is reserved for the future host-driven EN / reset line. There is no 3V3
wire -- the C6 is self-powered over its own USB.

**Required DIP positions: SW4-1 OFF, SW4-2 OFF, SW4-3 ON, SW4-4 OFF.** These
are mechanical switches and getting them wrong is invisible: SW4-4 ON with
SW4-3 OFF leaves the Octo-SPI flash owning the Pmod1 SPI pins and the U6 / U9
bus switches open, so J26-1..J26-4 never reach the MCU while every part
involved looks healthy. SW4-4 OFF also takes the Arduino and mikroBUS
connectors offline.

### Config gotcha: which pin leaves are settable

In `sdkconfig.defaults` the **settable** SPI pin leaves are
`CONFIG_ESP_SPI_HSPI_GPIO_{CS,MOSI,MISO,CLK}`. The similarly named <!-- LEGACY-OK: esp-idf sdkconfig key names; MOSI/MISO are upstream -->
`CONFIG_ESP_SPI_GPIO_{CS,MOSI,MISO,CLK}` symbols are **derived from those and <!-- LEGACY-OK: esp-idf sdkconfig key names; MOSI/MISO are upstream -->
are ignored** if you set them directly -- edit the `HSPI` leaves. By contrast,
`CONFIG_ESP_SPI_GPIO_DATA_READY` and `CONFIG_ESP_SPI_GPIO_HANDSHAKE` are
directly settable.

## Build

The dev box does **not** have esp-idf installed -- build on the Pi bench host
(the only machine with esp-idf). The build is fully reproducible from the pins:

```sh
./coprocessor/esp32c6/build.sh
```

`build.sh` requires `idf.py` on PATH and asserts esp-idf `v5.5.x`, then:

1. clones esp-hosted-mcu and checks out the pinned commit `949bb30`,
2. copies our `sdkconfig.defaults` into the upstream peripheral-side project,
3. cleans (`rm -rf build sdkconfig dependencies.lock managed_components`),
4. runs `idf.py set-target esp32c6 && idf.py build`,
5. prints the four output `.bin` paths.

The fetched clone lands at `coprocessor/esp32c6/esp-hosted-mcu/` and is git-ignored
(SOUP, fetched at build time -- never committed).

## Flash

Flash over the **CH343 USB-UART bridge** (VID:PID `1a86:55d3`). Do **not** use
the C6 native USB-JTAG interface to write the image: it fails with `EPIPE`
partway through `write_flash`.

Both interfaces enumerate as `/dev/ttyACM<n>`, and the numbering changes on a
power cycle -- on 2026-07-27 the bridge moved from `ttyACM1` to `ttyACM0` and
the board console took `ttyACM1`. `flash.sh` therefore resolves the bridge by
device identity (`/dev/serial/by-id/usb-1a86_USB_Single_Serial_*`, see
`scripts/hil/lib/tty_resolve.sh`) and fails loudly if it cannot, rather than
writing to whatever holds a number.

```sh
./coprocessor/esp32c6/flash.sh                 # resolve the CH343 bridge
./coprocessor/esp32c6/flash.sh <device>        # explicit port
```

The flash-size / mode / freq are pinned to the proven `16MB` / `dio` / `80m`
values in `pins.env`; a wrong flash size is the other common bring-up failure.

## Layout

| File | Role | Purpose |
|------|------|---------|
| `pins.env` | **SOURCE OF TRUTH** | Pinned versions + SPI pin map + flash params |
| `sdkconfig.defaults` | **DERIVED, verified against `pins.env`** | The same pin numbers in Kconfig syntax; byte-stable, as built on the bench |
| `build.sh` | consumer | Verify the two agree, fetch pinned upstream, apply config, clean, build |
| `flash.sh` | consumer | Flash the four artifacts over the CH343 bridge |

### Why the pin numbers appear twice

Because the two consumers cannot read the same syntax. `pins.env` is a plain
`KEY=value` fragment so `build.sh` and `flash.sh` can `source` it, and so one
file answers "what is wired to what" for anyone working the RA8 side.
`sdkconfig.defaults` is Kconfig, which is the *only* form esp-idf reads.

`sdkconfig.defaults` is therefore a **derived artifact**, but it is not
generated: it is kept byte-stable because the bench-proven C6 image was built
from exactly these lines, and regenerating it would put an unproven file in the
path of a working firmware. It is *verified* against `pins.env` instead.

That verification is a gate, not a convention, because this particular drift is
undetectable downstream -- the build succeeds, the image flashes, the board
boots, and the SPI link simply never comes up, because the two ends are driving
different pins. `scripts/checks/check_c6_pin_config.py` compares every SPI
signal, the chip target and the flash size; it runs in the `pre-commit-checks`
CI gate and in `scripts/git/pre-commit` (pure text compare -- no esp-idf
needed), and `build.sh` runs it on the bench before it builds.

**To change a pin: edit `pins.env`, then update `sdkconfig.defaults` to match,
and reflash the C6.** A board still holding the previous image keeps the
previous pins no matter what the repository says.

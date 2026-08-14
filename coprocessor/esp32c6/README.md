# ESP32-C6 wireless co-processor firmware

The EK-RA8D2 gains Wi-Fi by pairing the RA8D2 host with an **ESP32-C6** acting
as a wireless co-processor (Bluetooth over the same link is planned, not
delivered -- the host `drivers/bt/` bridge is excluded from the build and the
BLE HCI seam is still on loopback, #493). The C6 runs Espressif's
**esp-hosted-mcu** `network_adapter` application (the peripheral-side, or
upstream "slave", firmware) over a SPI transport; the RA8 is the host that <!-- LEGACY-OK: names the upstream esp-hosted-mcu role verbatim; our term is peripheral-side -->
drives it.

The C6 image is now a mixed image: pinned Espressif esp-hosted-mcu SOUP plus
the first-party `ra8_mdl_service` component. `build.sh` applies one checked-in
patch to the exact upstream commit to expose a bounded synchronous CustomRpc
response hook, stages the component from `port/esp32_c6`, and verifies the
service symbol exists in the final ELF. If the patch drifts, the build stops
before compilation. The RA8-side host driver lives in
`libs/third_party/esp-hosted/`, `port/esp-hosted/`, and `libs/ra8_c6link/`.

The media service performs a pull-based HTTPS transfer. Start returns quickly,
each Next request provides backpressure and acknowledges its offset, and HTTP
redirects are refused so an HTTPS request cannot downgrade to plaintext. The C6
counts every received byte and emits COMPLETE only after ESP-IDF confirms the
message body is complete and any advertised length matches that count. The RA8
remains the sole owner of SD paths, temporary files, and final rename.

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

### Required SW4 DIP positions

**SW4-1 OFF, SW4-2 OFF, SW4-3 ON, SW4-4 OFF.** These are four *independent*
mechanical switches, each owning a different thing:

| Switch | Required | What it owns |
|--------|----------|--------------|
| SW4-1 | OFF | Pmod1 Mode Select 1 |
| SW4-2 | OFF | Pmod1 Mode Select 2; OFF+OFF selects the SPI position (UM Table 18 p 26) |
| SW4-3 | **ON** | The **Octo-SPI mux**. ON is "Octo-SPI Inactive", which is what makes the U6 / U9 bus switches connect `P801`..`P804` to J26 at all (UM Table 3 p 16) |
| SW4-4 | **OFF** | The **Arduino / mikroBUS connectors** -- nothing to do with the Octo-SPI mux. `SW4-3 ON + SW4-4 ON is not a valid combination` (#555) |

Getting the bank wrong is invisible. With **SW4-3 OFF** the on-board Octo-SPI
flash keeps the Pmod1 SPI pins and the bus switches stay open, so
J26-1..J26-4 never reach the MCU while every part involved looks perfectly
healthy -- the host clocks transactions into `0xff` and each app reports a
protocol or timeout failure of its own flavour. Nothing says "the bus is not
connected". That reads as a firmware red and is not one; #555 tracks making
`make hil-c6` detect and abort on it.

Flipping **SW4-4 OFF** takes the Arduino and mikroBUS connectors offline, so
the LSM6DSO IMU Click is unavailable while the C6 link runs. That is a real
trade-off on this board, not an oversight, and it is why this tier cannot share
a bench pass with `make hil-all`.

The 2026-07-26 outage was a **misread of this bank** (SW4-4 ON, SW4-3 OFF), not
a wiring fault; the measurements taken while the nets were open at the mux are
preserved in
[`../../examples/ek_ra8d2/hw_validated/c6/c6_spi_probe/README.md`](../../examples/ek_ra8d2/hw_validated/c6/c6_spi_probe/README.md)
as a worked example of how convincing a correct measurement is when it is
attached to a wrong premise. Check the switches **electrically** before
suspecting anything else.

### Config gotcha: which pin leaves are settable

In `sdkconfig.defaults` the **settable** SPI pin leaves are
`CONFIG_ESP_SPI_HSPI_GPIO_{CS,MOSI,MISO,CLK}`. The similarly named <!-- LEGACY-OK: esp-idf sdkconfig key names; MOSI/MISO are upstream -->
`CONFIG_ESP_SPI_GPIO_{CS,MOSI,MISO,CLK}` symbols are **derived from those and <!-- LEGACY-OK: esp-idf sdkconfig key names; MOSI/MISO are upstream -->
are ignored** if you set them directly -- edit the `HSPI` leaves. By contrast,
`CONFIG_ESP_SPI_GPIO_DATA_READY` and `CONFIG_ESP_SPI_GPIO_HANDSHAKE` are
directly settable.

### Config gotcha: `CONFIG_ESP_HOST_DEV_BOARD_NONE=y` decides whether ANY of it applies

`sdkconfig.defaults` carries one line that is easy to read as boilerplate and
is not:

```
CONFIG_ESP_HOST_DEV_BOARD_NONE=y
```

esp-hosted-mcu ships Kconfig **presets for Espressif's own dev boards**, and
selecting one **overrides the SPI pin leaves**. So with any board but `NONE`
chosen, every pin in the tables above is silently ignored by the build and the
C6 comes up on the preset's pins instead of ours. The failure is the same
expensive shape as a pin typo: the build succeeds, the image flashes, the board
boots, and the link never comes up.

It is therefore recorded in `pins.env` as `C6_DEV_BOARD=none` and compared by
`scripts/checks/check_c6_pin_config.py`, which parses the enabled
`CONFIG_ESP_HOST_DEV_BOARD_<X>` symbol out of the **key name** (the same way it
reads `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`) and checks the suffix. Comparing the
suffix rather than merely asserting the symbol exists is what catches the
worse of the two drifts -- the selection *moving to a different board*, which
still produces a working build driving the wrong pins -- as well as the
selection vanishing.

## Toolchain

The build needs esp-idf, and **the dev box does not have it**: the Pi bench
host is the build machine. Provisioning is declared, not hand-built, in
[`../../infra/ansible/roles/c6_toolchain/`](../../infra/ansible/roles/c6_toolchain/)
-- the IDF checkout, its pinned release, the apt prerequisites (`python3-venv`
is the one whose absence hard-fails IDF's installer), and the per-minor-version
venv marker that keeps a re-run from re-downloading ~2 GB of toolchain.

The role also drops `/etc/profile.d/ra8-esp-idf.sh`, which gives an interactive
shell **`get_idf`** to source the IDF export script:

```sh
get_idf                        # or: . "${IDF_PATH}/export.sh"
idf.py --version               # must print exactly v5.5.4
```

**The version pin is not cosmetic: v5.4.1 does NOT build esp-hosted-mcu.** Its
component-manager pull of tf-psa-crypto fails to compile p256-m under that
release. `v5.5.4` is the release the bench proved end to end -- peripheral
firmware 2.12.11, flashed, alive, RPC round-trip green -- so provisioning must
land on it and not on the older pin. `build.sh` asserts that exact version
(not merely the `v5.5.x` series) before it does anything else.

## Build

Build on the Pi bench host (see **Toolchain** above -- the dev box has no
esp-idf). The build is fully reproducible from the pins:

```sh
./coprocessor/esp32c6/build.sh
```

`build.sh` requires `idf.py` on PATH and asserts esp-idf is exactly
`ESP_IDF_VERSION` from `pins.env`, then:

1. verifies `sdkconfig.defaults` still agrees with `pins.env`
   (`scripts/checks/check_c6_pin_config.py` -- the same script CI runs),
2. clones esp-hosted-mcu and checks out the pinned commit `949bb30`,
3. checks and applies the reviewed CustomRpc hook patch, pins the upstream MQTT
   dependency to the proven `1.0.0` release, then stages the first-party
   `ra8_mdl_service` component,
4. copies our `sdkconfig.defaults` into the upstream peripheral-side project,
5. cleans (`rm -rf build sdkconfig dependencies.lock managed_components`),
6. runs `idf.py set-target esp32c6 && idf.py build` and asserts the strong
   media-service handler (not the weak upstream fallback) and the component ABI
   marker both exist in the resulting ELF,
7. diffs the component set that actually resolved against
   `components-lock.txt` and fails on any difference,
8. prints the four output `.bin` paths.

There is no `menuconfig` step. See **No menuconfig** below.

The fetched clone lands at `coprocessor/esp32c6/esp-hosted-mcu/` and is git-ignored
(SOUP, fetched at build time -- never committed).

### Reproducibility: recipe vs bits

Step 4 deletes `dependencies.lock`, so the esp-idf component manager
re-resolves from the registry on every run. That alone makes the build
*recipe*-reproducible but not *bit*-reproducible -- the registry can hand back
a different component version tomorrow from an unchanged recipe.

The generated lock cannot simply be committed: it records a **host-specific
absolute path** for the local `cmd_system` component, taken from the building
machine's own IDF checkout, which would be wrong on every other host. What is
portable is the resolved **set**, so `components-lock.txt` records the kind,
name, version and registry hash of each component as resolved by the bench
build whose `network_adapter.bin` is the image actually flashed and proven. Step
6 normalises the fresh lock to the same four columns and diffs it, comparing the
local component on kind and version only and never on its path.

A difference there is not a warning to click past. The recipe is unchanged, so
it means the registry resolved something else -- either re-pin deliberately
(update the record **and** reflash and re-qualify the C6) or find out why.

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
The cabling itself is recorded there too (`C6_FLASH_BRIDGE*`,
`C6_FLASH_VIA_NATIVE_USB_JTAG`, `C6_POWER_SOURCE`) rather than living only in
this prose.

## Verify

Flashing is not evidence. The C6 is powered by its own USB, so it enumerates
and looks alive whether or not the harness or the SW4 mux is right -- which is
exactly the trap the 2026-07-26 outage fell into. Prove the image on the bench:

```sh
make hil-c6                          # the whole C6 tier, in triage order
make hil-c6 APP=c6_fw_version        # just the protocol round-trip
```

Bench requirements: **SW4 1=OFF 2=OFF 3=ON 4=OFF**, the harness on J26, the C6
on its own USB (`mk/hil.mk`, target `hil-c6`).

The app that settles "did the right firmware land" is
[`../../examples/ek_ra8d2/hw_validated/c6/c6_fw_version`](../../examples/ek_ra8d2/hw_validated/c6/c6_fw_version):
it sends a real esp-hosted RPC, decodes the response with the vendored protobuf
codec, and asserts the co-processor's reported version against
`ESP_HOSTED_VERSION_{MAJOR,MINOR,PATCH}_1` from the **vendored host driver** --
`2.12.11`, the same value `pins.env` pins as `ESP_HOSTED_MCU_FW_VERSION` and
that `check_c6_pin_config.py` holds the two ends to. Bump the vendor pin
without reflashing the C6, or reflash without bumping the pin, and that app
goes red on purpose instead of the mismatch surfacing later as unexplained RPC
timeouts.

Run the tier in order when triaging: `c6_spi_probe` (the wire), `c6_hosted_init`
(the port), `c6_fw_version` (the protocol). See
[`../../examples/ek_ra8d2/hw_validated/c6/README.md`](../../examples/ek_ra8d2/hw_validated/c6/README.md).

### No menuconfig

**There is no `idf.py menuconfig` step in this procedure, and running one is a
mistake.** The configuration is `sdkconfig.defaults` plus the checker that
verifies it against `pins.env` -- two committed files and a gate, which is why
a pin change is reviewable and a pin drift is catchable.

`menuconfig` writes a `sdkconfig`, which is *not* committed and which
`build.sh` deletes on the next run (step 4). So a change made that way survives
exactly until someone rebuilds, and in the meantime the tree says one thing
while the flashed image does another. To change a setting, edit
`sdkconfig.defaults` (and `pins.env`, if it is a pin), then rebuild and
reflash.

## Layout

| File | Role | Purpose |
|------|------|---------|
| `pins.env` | **SOURCE OF TRUTH** | Pinned versions + SPI pin map + RA8 landing pins + SW4 positions + flash params + bench USB cabling |
| `sdkconfig.defaults` | **DERIVED, verified against `pins.env`** | The same pin numbers in Kconfig syntax; byte-stable, as built on the bench |
| `components-lock.txt` | record, verified after each build | The esp-idf component set the proven image resolved. A RECORD, not a drop-in `dependencies.lock` -- see **Reproducibility** above |
| `build.sh` | consumer | Verify the two agree, fetch pinned upstream, apply config, clean, build, verify the resolved components |
| `flash.sh` | consumer | Flash the four artifacts over the CH343 bridge |
| `wifi.env.example` | template | Template for `wifi.env` (gitignored): `RA8_C6_WIFI_SSID` / `RA8_C6_WIFI_PSK`, compiled in by `examples/ek_ra8d2/hw_validated/c6/c6_wifi_join/Makefile`. The real passphrase lives in OpenBao at `secret/ra8d2/bench-network` (key `bench_psk`), never here |

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
signal, the chip target, the flash size and the dev-board preset; it runs in
the `pre-commit-checks` CI gate and in `scripts/git/pre-commit` (pure text
compare -- no esp-idf needed), and `build.sh` runs it on the bench before it
builds.

**To change a pin: edit `pins.env`, then update `sdkconfig.defaults` to match,
and reflash the C6.** A board still holding the previous image keeps the
previous pins no matter what the repository says.

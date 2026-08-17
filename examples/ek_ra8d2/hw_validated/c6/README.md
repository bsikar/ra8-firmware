# examples/ek_ra8d2/hw_validated/c6/

Apps that talk to the **ESP32-C6 companion radio** over esp-hosted. Every one
has been run on real silicon and passes. They live in their own tier because of
the bench, not because of their maturity: `make hil-c6` runs the lane through
the same runner and the same per-app `hil.conf` manifests as the default suite,
because there is one HIL runner in this tree, not one per bench configuration.

## Why they are not in `hw_validated/hil/`

Two independent reasons, either sufficient on its own.

**The DIP switches are mutually exclusive with the default pass.** These apps
need SW4 **1=OFF 2=OFF 3=ON 4=OFF**. SW4-3 ON deactivates Octo-SPI, which is
what connects the SPI pins through the bus switches to J26 at all; SW4-4 OFF
deactivates the Arduino and mikroBUS connectors, which the rest of the tree's
apps need. One run of the bench cannot satisfy both settings.

> Getting SW4-3 wrong leaves the J26 signal holes electrically disconnected
> from the MCU while the board and the co-processor both look perfectly
> healthy. That misreading -- not a wiring fault -- cost an entire bench day.
> If the link stops working, check the switch bank **electrically** before
> suspecting anything else.

**`ra8_emulator` models no ESP32-C6** (#494). `hw_validated/hil/` is bound by
`check_hil_eil_parity.py` to the EIL suite: every app there must also be
exercised in the emulator, with no skips. That gate is right and these apps
genuinely cannot satisfy it, so putting them there would mean either a failing
gate or a hole punched in one. They stay outside it until the emulator grows a
co-processor model.

## They form a ladder

`c6_spi_probe` is the bench's negative control and is the first thing to run,
always. It drives the SPI pins directly -- no port, no vendored driver -- so
when something higher up fails it separates "the wire is wrong" from "the
firmware is wrong". Above it the apps climb through the vendored host port on
silicon, a hand-built RPC round trip, the `ra8_c6link` facade, a real network
association with DHCP, the `ra8_wifi` HAL over that same path, and finally
camera frames served over the radio.

Two rungs overlap deliberately: one builds the protocol by hand and the next
goes through the facade, so when one passes and the other fails, the difference
is the facade rather than the link.

## Bench setup

The harness is stripped 22AWG jumpers between J26 and the C6 dev board's
headers. There is **no 3V3 wire**: the C6 is powered from its own USB, and
J26-6 / J26-12 must stay empty.

The pin map lives in exactly two places -- the RA8-side pins header under
`port/esp-hosted/` and `coprocessor/esp32c6/pins.env` -- and
`scripts/checks/check_c6_pin_config.py` diffs them on every CI run so they
cannot drift apart. Edit those, never a table in a document.

Do not reflash the co-processor without a specific reason. The firmware-version
app asserts the running co-processor image against the vendored host driver's
own version, so a reflash without a matching vendor bump turns that test red on
purpose.

## Two bench facts that repeatedly cost time

**`/dev/ttyACM*` numbering swaps across power cycles.** The J-Link, the chip's
own USBHS CDC and the C6's USB-serial bridge all enumerate in that namespace.
Always address consoles through `/dev/serial/by-id/`; the HIL scripts already
do.

**The co-processor's boot announcements are one-shot.** It queues them at boot
and holds them until a transaction drains them, so whatever image is already in
MRAM eats them the moment the board powers up. To see them, flash a neutral app
first, power-cycle, then flash the app you actually want.

## See also

`docs/design/c6_wireless_architecture.md` for how the co-processor fits into
the system, and `coprocessor/esp32c6/README.md` for building and flashing its
image.

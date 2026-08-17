# c6_spi_probe

Bring-up instrument for the raw SPI link between the RA8D2 and the ESP32-C6 on
Pmod1 (J26). It drives the bus directly -- no port, no vendored driver --
hand-decodes the esp-hosted payload header out of the received bytes and prints
one verdict. It is the tier's negative control: when anything higher up fails,
this is what separates "the wire is wrong" from "the firmware is wrong", so it
is the first thing to run when triaging.

A PASS needs one received frame with recognisable esp-hosted structure, either a
verified data frame or the co-processor's idle filler. Both are trivially
distinguishable from a bus reading all-zero or all-ones.

## The measurement techniques, and why each one works

- **Side-band read with no internal pull held.** The C6 drives HANDSHAKE and
  DATA_READY push-pull, and a standing pull-up would make an unconnected pin
  look asserted.
- **Wire test.** Drive a net, release it to a no-pull input, sample, repeat for
  the other level. A net with nothing on it holds the driven level on its own
  capacitance; a terminated one snaps back.
- **Chip-select hunt with no clock and no payload.** The co-processor image
  deasserts HANDSHAKE on the chip-select falling edge, so asserting each
  candidate in turn finds both the chip-select pin and the HANDSHAKE pin without
  transferring a single byte.
- **Pull-up contest, run last.** Re-reading each side-band pin with the MCU's
  internal pull-up engaged identifies a pin held low by a real off-chip driver:
  losing a current fight is something nothing floating can imitate. It has to
  run after the transaction sweep, because a freshly-booted C6 holds DATA_READY
  high until the first transaction drains its queued boot event.

That last technique exists because DATA_READY defeats an edge-counting heuristic
entirely: it transitions once per boot, between transactions, so a
per-transaction vote scores it zero forever. In the run that taught this, a
floating unconnected hole won the DATA_READY vote outright while the real pin
scored nothing at all.

## Board facts this app settled

The Pmod1 mux puts either of two MCU pins on J26-1 depending on the switch bank
(EK-RA8D2 v1 UM Table 18 p 26), and the on-board Octo-SPI flash releases those
pins only when SW4-3 is ON (UM Table 3 p 16). With that switch wrong, the J26
signal holes are not electrically connected to the MCU at all while the board
and the co-processor both look perfectly healthy -- which is how a bench day was
once spent suspecting the wiring. Flipping SW4-4 OFF for this tier also
deactivates the Arduino and mikroBUS connectors, so the IMU Click is offline
whenever the C6 link is in use: a real trade-off on this board, not an oversight.

There is no 3V3 wire in the harness -- the C6 runs from its own USB -- which is
exactly why "the C6 is alive" was never evidence that the signal harness
existed.

## Protocol constants

Nothing from esp-hosted-mcu is vendored into this app. Every protocol constant
is hand-decoded from the pinned upstream tree and cites the upstream file it
came from in its Doxygen block, as do the link parameters -- SPI mode, checksum
enabled, deassert-handshake-on-chip-select -- which are taken from the
co-processor image's own generated config.

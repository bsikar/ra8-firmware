# dfu_copy_to_run

Unattended proof of the `dfu_bootloader`'s copy-to-run scheme (issue #97): an
image linked **once** at the SRAM run base runs from wherever it is staged, so
there is no per-slot build and no "which slot am I building for?" footgun.

The app embeds a demo payload, hands it to the same `ra8_dfu_launch` the
bootloader uses -- run-target check, copy, then the VTOR / MSP / branch hand-off
-- and the payload spins in the SRAM run window writing a sentinel and an
advancing heartbeat.

That heartbeat is what makes the gate specific rather than generic. This app's
own code lives in MRAM, so the SRAM run window is reachable **only** by having
copied there and branched, and a counter advancing inside it cannot be produced
any other way. If the run-target check fails, control returns and parks in a
panic halt, which the gate flags as a fault spinner. The bootloader's own gate
is weaker because it can also pass via the DFU-device fallback; this app always
copies to run.

`examples/ek_ra8d2/hw_validated/hil/dfu_copy_to_run/scripts/build_payload.sh`
regenerates `inc/payload_image.h` from `src/payload.c`; commit the refreshed header
whenever the payload changes. The DFU *programming* path that
fills a slot is covered separately by the `dfu_selftest_*` self-loop twins.

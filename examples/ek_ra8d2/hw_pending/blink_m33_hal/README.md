# blink_m33_hal

The HAL-based twin of `blink_m33` (#580): the Cortex-M85 releases the
Cortex-M33 and sleeps, the M33 blinks LED1. Everything is identical except
**how the M33 drives the pin**.

`blink_m33` hand-rolls the combined `{PODR, PDR}` PCNTR1 store from a raw
address cast. This app calls `ra8_pcntr_set_output()` instead. That primitive
and its reader `ra8_pcntr_read()` live header-only in `libs/ra8_hal`, built
directly on the PORT register layer with **no** pin-validator, logging or PFS
dependency -- which is what lets them compile under the M33 image's
`-ffreestanding` build and link nothing from `ra8_hal`.

The primitive read-modify-writes PCNTR1, so it preserves every other pin on the
port; the bare full-word store it replaces would clear the siblings. At a
single-pin call site the observable LED behaviour is identical, so it is a
drop-in substitute that is additionally safe on a shared port.

The primitive is host-unit-tested (`tests/hal/src/test_ra8_pcntr.c`), and the example's
own blink step -- the exact function the CPU1 loop runs -- is driven against
fake MMIO in `tests/mocks/src/test_app_blink_m33_hal.c`. Under the emulator only the
primary core's ITM is echoed, so the M33 stays silent by design and LED1 is its
output.

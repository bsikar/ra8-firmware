# blink_m33_hal -- Cortex-M33 secondary-core LED blink via the HAL PCNTR primitive

The HAL-based twin of [`blink_m33`](../../hw_validated/hil/blink_m33/) (issue
#580). Everything is identical -- the **Cortex-M85** releases the **Cortex-M33**
and sleeps, the M33 blinks LED1 -- except **how the M33 drives the pin**.

## The one difference

`blink_m33/cpu1_main.c` casts `0x404000C0` to a `volatile uint32_t*` and
hand-rolls the combined `{PODR, PDR}` PCNTR1 store. This app calls the CPU1-safe
HAL primitive instead:

```c
(void)ra8_pcntr_set_output(k_ra8_port_6, k_ra8_pin_0, level);   /* LED1 = P600 */
```

`ra8_pcntr_set_output()` (and its reader `ra8_pcntr_read()`) live header-only in
[`libs/ra8_hal/inc/ra8_pcntr.h`](../../../../libs/ra8_hal/inc/ra8_pcntr.h). They
build directly on the PORT register layer (`ra8_port_regs.h`) with **no**
pin-validator, logging, or PFS dependency, so they compile clean under the M33
image's `-ffreestanding` build and link **nothing** from `ra8_hal`.
`ra8_add_cpu1_image()` simply adds `libs/ra8_hal/inc` to the M33 include path.

The primitive read-modify-writes PCNTR1 so it preserves every other pin on the
port (a bare full-word store, the ad-hoc form, would clear the siblings). At the
single-pin CPU1 call site the observable LED behaviour is identical, so it is a
drop-in substitute -- while being safe to reuse on a shared port.

## Run it

```sh
make emu-blink_m33_hal      # both cores in ra8_emulator, with the live board view
make blink_m33_hal          # cross-compile blink_m33_hal.elf (+ blink_m33_hal_cpu1.elf)
```

Watch **LED1 (BLUE)** toggle; the `[itm]` stream shows the M85's release log and
then goes quiet as the M85 idles (ra8_emulator echoes only the primary core's
ITM, so the M33 stays silent by design -- the LED is its output).

## Memory map

| Region    | Origin     | Size   | Holds                          |
|-----------|------------|--------|--------------------------------|
| MRAM      | 0x02000000 | ...    | M85 code + the `.cpu1_image`   |
| MRAM_CPU1 | 0x020C0000 | 256 K  | M33 code + rodata + vectors    |
| SRAM_CPU1 | 0x22190000 | 64 K   | M33 `.data`, `.bss`, stack     |

## Status

`hw_pending` -- the raw `blink_m33` is the HIL-validated reference; this HAL
variant is behaviourally identical (same LED1 blink, same PCNTR1 effect). The
primitive is host-unit-tested (`tests/test_ra8_pcntr.c`) and the example's own
blink step is host-tested end to end (`tests/test_app_blink_m33_hal.c` drives the
shared `blink_m33_hal.h` step -- the exact function this app's CPU1 loop runs --
against the fake MMIO and asserts the LED1 PCNTR1 effect). This exact ELF has not
had its own bench run, so it is not promoted; the emulator (`make
emu-blink_m33_hal`) runs both cores with the live LED1 board view.

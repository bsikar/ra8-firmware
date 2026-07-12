# acmphs_compare

High-Speed Analog Comparator (ACMPHS) channel-0 polling demo for the
bare EK-RA8D2 EVM. Initializes ACMPHS0, polls the comparator output via
`ra8_acmphs_read_output`, toggles LED1 each time the output is HIGH and
LED2 each time the output is LOW. LED3 reports init failure.

Build / flash:

```
make acmphs_compare
make -C examples/ek_ra8d2/acmphs_compare flash
```

## HIL plan

**Requires physical stim -- needs analog ground-truth on IVCMP /
IVREF.** The comparator output depends on the voltage applied to the
ACMPHS0 input pin relative to its reference; on a bare EVM that pin
floats. The only externally-observable signal is LED1 vs LED2
toggling, which only proves the chip is alive, not that the
comparator decision was correct.

No UART output is emitted, so `uart_scrape` is not applicable. A
`hil_check_alive` could prove the firmware did not HardFault, but
would not assert anything about the analog path.

To make this HIL-able: tie IVCMP to a known reference (e.g. wire it
to LED1's GPIO so the comparator output is deterministic) and add a
`g_acmphs_high_count` / `g_acmphs_low_count` pair of volatile globals
for a `jlink_memprobe` mode. That requires bench wiring.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the comparator path.

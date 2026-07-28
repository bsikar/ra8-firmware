# adc_diag_tsn_demo

Runs the ADC_B built-in self-diagnosis (modes 1/2/3) and the
close-the-loop on-die temperature read once per cycle, logging results
over SCI8 (PD_02 / PD_03 -> J-Link OB CDC). Drives LED1 as a heartbeat.

Exercises the safety-relevant analog HAL entry points added for the
driver-mode gap issues:

- `ra8_adc_self_diagnose` -- reference self-test, modes 1/2/3 (#182).
- `ra8_tsn_read_die_temp_milli_c` -- end-to-end die temperature (#183).
- `ra8_adc_read_internal_channel` -- internal Vref channel probe (#183).

Console output per cycle:

    diag: mode=1 code=00000 PASS
    diag: mode=2 code=32768 PASS
    diag: mode=3 code=32767 PASS
    tsn: die_mC=42000
    vref: raw=2730
    diag: selftest PASS

hw_pending: `ra8_emulator` models neither the SAR self-test (ADEXDR /
ADSGDCR.DIAGVAL) nor the temperature-sensor path (TSCR), so the demo is
bench-only. Needs a real EK-RA8D2 with the J-Link OB CDC console.
Promote to `hw_validated/hil/` once confirmed on silicon.

Build:

```
make adc_diag_tsn_demo
```

# pdm_mic_demo

Capture + plausibility demo for the EK-RA8D2 on-board SPH0690 PDM MEMS
microphones, driving the `ra8_pdm` (PDM-IF) driver. Closes gap issue #129.

The app brings up PDM-IF channel 2, captures 20-bit PCM windows, computes
AC RMS / peak / span and prints a plausibility verdict scraped by
`hil.conf`:

```
pdm: init ch=2 clk=4mhz fs=16khz
pdm: rms=612 peak=3480 mean=-3 span=6210 vary=180 active=Y
pdm: rms=598 peak=3120 mean=-2 span=5904 vary=180 active=Y
...
```

`active=Y` means the captured audio is non-degenerate (not a stuck DC
constant, not all-zero) and carries plausible acoustic energy
(`span >= 8` and `rms >= 16` LSB of the +-524288 full scale). A dead line
would print `active=N`, and a non-toggling PDM clock would print
`pdm: no data (FIFO empty) -- clock/mic?`. Make noise or tap near the
underside mics and `rms` / `peak` track it; the streamed `vary` is the
RMS spread seen across the first measurement sweep.

The verdict is also latched in the J-Link-probable global
`g_pdm_mic_result` (`magic` = `0x50444D31` once the first window
completes).

## Hardware

The EK-RA8D2 carries two SPH0690LM4H-1 PDM MEMS microphones (MIC1, MIC2)
on the **underside** of the PCB. No external hardware is required.

| SPH0690 pin | Signal | EVM net | MCU function |
| ----------- | ------ | ------- | ------------ |
| 1 (DATA)    | PDM data  | P502 | PDMDAT2 (PSEL 0x1B) |
| 4 (CLOCK)   | PDM clock | P812 | PDMCLK2 (PSEL 0x1B) |
| 2 (SELECT)  | L/R sel   | GND (MIC1) / +3.3V (MIC2) | rise / fall edge |

Both mics share the single clock and data line (standard PDM stereo), so
they are on PDM-IF **channel 2**. `INPSEL=0` selects the rise-edge mic
(MIC1, SELECT=GND). Reference: EK-RA8D2 v1 UM Table 31 p 37; RA8D2
datasheet pin functions for P502/P812.

## Signal path (all hardware, HUM Ch 49)

```
PDMIFCLK = MOCO 8 MHz
  -> PDM_CLK2 = 8 MHz / (2*(CKDIV+1)) = 4 MHz          (CKDIV=0)
  -> 4th-order sinc decimation, M = SINCDEC+1 = 125     (SINCDEC=0x7C, SINCRNG=0x05)
  -> compensation FIR -> high-pass (DC block) -> half-band /2
  -> Fout = 4 MHz / (2*125) = 16 kHz, 20-bit signed PCM (PDDRRCH2)
```

Decimation combo is a HUM Table 49.7 row (order 4). The filter
coefficients are the RA8D2 reset-default SPH0690 set (HUM Ch 49.2
register reset values), written explicitly in `k_pdm_demo_cfg`.

## Build / run

```
cd examples/ek_ra8d2/hw_pending/pdm_mic_demo
make                 # -> build/pdm_mic_demo.elf / .hex
bash ../../../../scripts/hil/flash.sh pdm_mic_demo
# scrape SCI8 console @115200:
ssh star 'stty -F /dev/ttyACM0 115200 raw -echo; timeout 8 cat /dev/ttyACM0 | grep -a "pdm: rms="'
```

## Status

`hw_pending`: builds for the target and is driven end-to-end against the
`ra8_pdm` register model in the host test (`tests/test_ra8_pdm.c`). Promote
to `hw_validated/hil` once the `pdm: rms=... active=Y` banner is confirmed
on a bench EK-RA8D2.

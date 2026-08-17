# pdm_mic_demo

Captures from the EK-RA8D2's on-board PDM MEMS microphones through the
`ra8_audio` facade and its `ra8_pdm` backend, then judges whether what came
back is plausibly acoustic. Closes gap issue #129.

The verdict is the point. It captures 20-bit PCM windows and reports RMS, peak,
mean and span, calling the line active only when a window is non-degenerate --
not a stuck DC constant, not all zero -- and carries plausible energy. A dead
mic line reads as inactive; a PDM clock that never toggles leaves the FIFO
empty and says so. That is a distinction "did the driver return ok?" cannot
make. After the terminal verdict the firmware parks in a low-power wait so the
bench and the emulator observe one stable result, and the verdict is also
latched in the J-Link-probable global `g_pdm_mic_result`.

## Hardware

Two SPH0690LM4H-1 PDM MEMS microphones sit on the **underside** of the PCB. No
external hardware is required.

| SPH0690 pin | Signal    | EVM net                   | MCU function        |
| ----------- | --------- | ------------------------- | ------------------- |
| 1 (DATA)    | PDM data  | P502                      | PDMDAT2 (PSEL 0x1B) |
| 4 (CLOCK)   | PDM clock | P812                      | PDMCLK2 (PSEL 0x1B) |
| 2 (SELECT)  | L/R sel   | GND (MIC1) / +3.3V (MIC2) | rise / fall edge    |

Both mics share the single clock and data line (standard PDM stereo), so both
land on PDM-IF channel 2 and `INPSEL` chooses between them by clock edge --
`INPSEL=0` selects the rise-edge mic, MIC1 with SELECT tied to GND. Pin
assignments follow EK-RA8D2 v1 UM Table 31 p 37 and the RA8D2 datasheet pin
functions for P502 / P812.

## Signal path (all hardware, HUM Ch 49)

```
PDMIFCLK = MOCO 8 MHz
  -> PDM_CLK2 = 8 MHz / (2*(CKDIV+1)) = 4 MHz          (CKDIV=0)
  -> 4th-order sinc decimation, M = SINCDEC+1 = 125     (SINCDEC=0x7C, SINCRNG=0x05)
  -> compensation FIR -> high-pass (DC block) -> half-band /2
  -> Fout = 4 MHz / (2*125) = 16 kHz, 20-bit signed PCM (PDDRRCH2)
```

That decimation combination is a HUM Table 49.7 row (order 4). The filter
coefficients are the RA8D2 reset defaults for this microphone family (HUM
Ch 49.2 register reset values), owned by the board's PDM adapter rather than by
this app.

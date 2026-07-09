# camera_capture

OV5640 5 MP camera capture self-test for the EK-RA8D2 (issue #119). Brings
up the OV5640 over the chip **Capture Engine Unit (CEU)** parallel (DVP)
path -- the interface the board wires the camera to (FFC port J35), not
MIPI-CSI -- captures one frame into internal SRAM, and reports a
plausibility verdict over SCI8.

## What it does

1. `ra_cgc_init` / `ra_time_init` / `ra_mstp_init`, then the BSP SCI8
   console (`ra_board_uart_console_init`, PD02 / PD03).
2. **XCLK**: GPT channel 12 saw-PWM on **GTIOC12A (P501)** synthesises the
   ~24 MHz sensor input clock (XVCLK). The OV5640 needs XVCLK before it
   answers on SCCB, so this comes up first. The app then confirms the GPT
   counter is advancing (`gpt=RUN`).
3. **Mode**: forces **SW4-6 = ON** through the U15 expander so the Camera
   Expansion Board is in parallel (DVP) mode (the board default is MIPI).
4. **SCCB**: RIIC channel 1 (SCL1 P512 / SDA1 P511, shared with U15 and the
   DA7212 codec). Scans the bus, releases the sensor RST strap (P709), then
   reads the OV5640 chip-ID registers `0x300A:0x300B` (trying SCCB 0x3C then
   0x3D) -- the VERIFY-FIRST proof the sensor is present (expected `0x5640`).
5. Routes the 11 J35 DVP pins (VIO_D[7:0], VIO_VD, VIO_HD, VIO_CLK) to the
   CEU and programs a compact OV5640 DVP YUV422 QVGA (320x240) sequence with
   the **built-in colour-bar test pattern** enabled -- deterministic
   regardless of lens focus or scene light.
6. `ra_ceu_init` + `ra_ceu_capture_arm` capture one frame into SRAM;
   completion is polled on `CETCR.CPE`.
7. Computes min / max / mean over the frame. **PASS** when the sensor ID is
   `0x5640`, a frame was captured, and it is non-degenerate (`max != min`).

## Banner

```
cam: gpt=RUN scan=3C:56.43:00. chipid=5640 xclk=OK rst=OK sccb=OK ceu=OK frame=OK min=.. max=.. mean=.. verdict=PASS
```

Result globals for SWD probing: `g_cam_chipid`, `g_cam_frame_ok`,
`g_cam_min`, `g_cam_max`, `g_cam_mean`, `g_cam_verdict`.

## Drivers exercised

`ra_ceu` (CEU, HUM Ch 60), `ra_gpt` (XCLK on GTIOC12A, HUM Ch 22),
`ra_i2c` (SCCB / RIIC ch1, HUM Ch 39), `ra_pfs`/`ra_gpio` (pin routing +
RST, HUM Ch 20), `ra_board_uart_console`.

## HIL status: BLOCKED -- OV5640 not detected on the bus

Bench-run 2026-07-09 on the rig (`ssh star`, J-Link). The observed banner:

```
cam:  gpt=RUN scan=1A:00.20:29.21:29.22:29.23:00.43:00. chipid=0000 xclk=OK rst=OK sccb=ERR verdict=FAIL
```

The **OV5640 does not answer on the SCCB bus** (no ACK at 0x3C or 0x3D),
so the capture path could not be validated. The bring-up was verified
sound end to end:

- **RIIC ch1 is healthy** -- six devices ACK: the DA7212 audio codec
  (0x1A), the U15 SW4-override expander (0x43), and unidentified devices
  at 0x20-0x23 (reg-0 fingerprints 0x29/0x29/0x29/0x00). None is an
  OV5640 (which would sit at 0x3C and read 0x56 at reg 0x300A).
- **XVCLK is confirmed running** -- the GPT ch12 counter advances
  (`gpt=RUN`), i.e. the P501 GTIOC12A square wave is live.
- **SW4-6 was forced to ON** (parallel/DVP) via U15 (output 0xF2 -> 0xD2).
- **RST (P709) was released** before probing (`rst=OK`).

Per EK-RA8D2 UM Tables 35 (parallel) and 36 (MIPI), the SCCB (P511/P512),
XCLK (P501) and RESET (P709) are identical in both camera modes -- only
the data path differs -- and there is **no camera power / PWDN signal on
the FFC** (the board is powered from the FFC rails when connected). With
XVCLK live, RST released, and a healthy bus, an electrically-present
OV5640 would ACK 0x3C. It does not.

**Conclusion:** the OV5640 Camera Expansion Board is not electrically
present on the underside FFC port (J35) -- most likely not seated /
connected, or the accessory in use is not the OV5640 DVP board. Re-seat
the Camera Expansion Board FFC (and confirm it is the OV5640 board, not a
USB webcam) and re-run; the app promotes to `hw_validated/hil/` with a
`uart_scrape` gate on `verdict=PASS` once a frame is captured.

## Hardware

EK-RA8D2 with the OV5640 Camera Expansion Board on the underside FFC port
(**J35**). SW4-6 is driven ON in firmware (no manual switch change needed).
The parallel (DVP) camera shares P405/P406 with the audio codec (J41) and
shares PB02-PB04 with the parallel-graphics port, so those are mutually
exclusive.

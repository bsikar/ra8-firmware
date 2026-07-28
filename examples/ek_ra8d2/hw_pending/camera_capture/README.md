# camera_capture

OV5640 5 MP camera capture self-test for the EK-RA8D2 (issue #119). Brings
up the OV5640 over the chip **Capture Engine Unit (CEU)** parallel (DVP)
path -- the interface the board wires the camera to (FFC port J35), not
MIPI-CSI -- captures one frame into internal SRAM, and reports a
plausibility verdict over SCI8.

## What it does

1. `ra8_cgc_init` / `ra8_time_init` / `ra8_mstp_init`, then the BSP SCI8
   console (`ra8_board_uart_console_init`, PD02 / PD03).
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
6. `ra8_ceu_init` + `ra8_ceu_capture_arm` capture one frame into SRAM;
   completion is polled on `CETCR.CPE`.
7. Computes min / max / mean over the frame. **PASS** when the sensor ID is
   `0x5640`, a frame was captured, and it is non-degenerate (`max != min`).

## Banner

Target (PASS, once a frame captures -- `cetcr=00000001` is CPE, frame done):

```
cam: gpt=RUN scan=...3C:56... chipid=5640 xclk=OK rst=OK sccb=OK ceu=OK frame=OK cetcr=00000001 min=.. max=.. mean=.. verdict=PASS
```

Actual on silicon (2026-07-09) -- sensor streams, CEU rejects the frame
(`cetcr=01120300` = IGHS+VBP+NHD+HD+VD); see the HIL status section:

```
cam: gpt=RUN scan=1A:00.20:29.21:29.22:29.23:00.3C:00.43:00. chipid=5640 xclk=OK rst=OK sccb=OK ceu=OK frame=TIMEOUT cetcr=01120300 min=0 max=0 mean=0 verdict=FAIL
```

Result globals for SWD probing: `g_cam_chipid`, `g_cam_frame_ok`,
`g_cam_min`, `g_cam_max`, `g_cam_mean`, `g_cam_verdict`.

## Drivers exercised

`ra8_ceu` (CEU, HUM Ch 60), `ra8_gpt` (XCLK on GTIOC12A, HUM Ch 22),
`ra8_i2c` (SCCB / RIIC ch1, HUM Ch 39), `ra8_pfs`/`ra8_gpio` (pin routing +
RST, HUM Ch 20), `ra8_board_uart_console`.

## HIL status: BLOCKED -- sensor streams, CEU rejects the frame

Bench-run 2026-07-09 on the rig (`ssh star`, J-Link `.env` `JLINK_SN`),
diagnosed by driving the CEU and reading PORT PIDR / CEU registers live
over SWD. **Earlier "OV5640 not detected on the bus" reports are
superseded:** the OV5640 now answers (chip ID `0x5640`) and **fully
streams over the parallel port** -- every DVP signal was confirmed
toggling on the MCU pins:

- **VIO_CLK (PCLK, PB04)** -- free-running, ~50% duty.
- **VIO_HD (HREF, PB03)** -- toggling, active-high, per active line.
- **VIO_D[7:0]** (P400 / P902 / P405 / P406 / P700-P703) -- all toggling:
  real pixel data on the bus.
- **VIO_VD (VSYNC, PB02)** -- detected by the CEU (CETCR.VD). The OV5640
  gates VD to the DVP port with **register 0x300E bit6**; the Linux DVP
  power-down value `0x300E=0x18` clears that bit and makes the CEU report
  NVD, so this app leaves 0x300E at its reset default.

Sensor config was corrected to canonical OV5640 DVP-QVGA values (ISP
scaler on `0x5001=0xA3`, binning `0x3821=0x07`, matched PLL/PCLK, DVP pad
enables `0x3017=0x7F` / `0x3018=0xFC`), and the CEU `CMCYR.HCYL`
byte-vs-pixel bug was fixed (data-synchronous fetch counts bus cycles, so
width = line bytes = 640, not 320 pixels).

Despite every signal being present, **each armed capture ends with**
`cetcr=01120300` and **zero bytes written** (verified by poisoning
`s_frame` before each arm -- the marker survives):

- **IGHS** (bit17) -- the VIO_HD active clock-cycle count differs from
  `CMCYR.HCYL`. `HCYL` was swept 2..2560 (coarse) and finely around 640;
  **no value clears IGHS**, so the HREF-active width the CEU measures is
  not a stable/matchable number.
- **VBP** (bit20) -- "invalid VD": the illegal HD leaves the frame-end
  undetectable, so the frame is discarded before any line is written.

The failure is **invariant** to every CEU polarity / edge / mode
(image-capture vs data-synchronous) / geometry and to every sensor
`0x4740` / `0x300E` value reachable over SWD -- i.e. it is a VIO_HD /
VIO_CLK **timing or signal-integrity** problem, not a register-config
one. Slowing the sensor PCLK (system divider `0x3035` /2 -> /8) does move
the boundary -- VBP clears at `HCYL=320` -- but **IGHS never clears at
any PCLK rate**, so the HREF-active count is not merely too fast for the
CEU, it is unstable line-to-line (jitter), which points at the VIO_HD
edge quality rather than the clock frequency. **Resolving it needs a logic analyzer** on VIO_HD + VIO_CLK to
measure the true HREF-active duration per line and the PCLK/HREF phase,
then match `CMCYR.HCYL` / `CAPWR` (or the sensor DVP timing) to it. The
bench Analog Discovery 2 is not wired to J35 and its SDK is not installed
on `star`.

The app promotes to `hw_validated/hil/` with a `uart_scrape` gate on
`verdict=PASS` once a frame is captured.

## Hardware

EK-RA8D2 with the OV5640 Camera Expansion Board on the underside FFC port
(**J35**). SW4-6 is driven ON in firmware (no manual switch change needed).
The parallel (DVP) camera shares P405/P406 with the audio codec (J41) and
shares PB02-PB04 with the parallel-graphics port, so those are mutually
exclusive.

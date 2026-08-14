# camera_capture

OV5640 live camera capture for the EK-RA8D2. The firmware selects the Camera
Expansion Board's parallel DVP path, captures one VGA frame through the RA8D2
Capture Engine Unit (CEU), stores packed YCbCr 4:2:2 in internal SRAM, and
reports a plausibility verdict over SCI8.

## Pipeline

1. Initializes clocks, timekeeping, module-stop control, and the SCI8 console.
2. Generates the OV5640's approximately 24 MHz XVCLK with GPT12 on P501 and
   verifies that the timer is running.
3. Uses RIIC1 to drive U15's SW4-6 override ON, selecting DVP without requiring
   a physical switch change.
4. Releases the OV5640 reset on P709, scans SCCB, and verifies chip ID `0x5640`
   at address `0x3C` or `0x3D`.
5. Programs the proven live-scene VGA (640x480) YUV422 sensor sequence and
   samples DVP sync/data activity while the pins are still GPIO inputs.
6. Routes VIO_D[7:0], VIO_VD, VIO_HD, and VIO_CLK to the CEU, then performs a
   single-shot data-synchronous capture into the 614400-byte SRAM buffer.
7. Invalidates the data cache before CPU access and computes min/max/mean byte
   statistics.
8. Converts UYVY to RGB888 in RA8 firmware C and writes clockwise 0/90/180/270
   views into four cache-cleaned external-SDRAM buffers. `PASS` requires both
   the CEU capture and all four RGB buffers.

The CEU buffer is packed Cb-Y0-Cr-Y1 (UYVY) after the configured input ordering
and byte swaps. The OV5640 supports native mirror/vertical-flip controls but no
documented 90/270 transpose, so 90/270 are firmware-side pixel rotations after
UYVY-to-RGB conversion. The host script does not rotate pixels; it dumps the
firmware buffers and adds only the PPM headers.

## Build

```sh
make -C examples/ek_ra8d2/hw_validated/hil/camera_capture
```

The output is `examples/ek_ra8d2/hw_validated/hil/camera_capture/build/camera_capture.elf`
with matching `.hex` and `.bin` files.

## Capture A Still

From a configured HIL host with the bench environment available:

```sh
scripts/hil/camera_picture.sh camera_capture.ppm
```

The command builds and flashes the firmware, requires `rgb=OK` and
`verdict=PASS`, dumps the raw CEU frame plus four firmware-produced RGB buffers
over SWD, and saves:

- `camera_capture.uyvy` -- raw packed 640x480 YCbCr 4:2:2.
- `camera_capture.ppm` -- unrotated RGB image.
- `camera_capture_90.ppm`, `camera_capture_180.ppm`, and
  `camera_capture_270.ppm` -- orientation choices for the mounted camera.

## Capture A Video

`ffmpeg` is required for MP4 encoding. Each video frame is a separate verified
firmware capture and SWD dump.

```sh
scripts/hil/camera_video.sh --frames 10 --fps 2 --output camera_capture.mp4
```

## Verdict

A successful run includes the DVP probe, captured byte count, CEU status, frame
statistics, and final verdict:

```text
cam: gpt=RUN scan=... chipid=5640 xclk=OK rst=OK sccb=OK dvp_sync_edges=... ceu=OK bytes=614400 frame=OK cetcr=........ min=.. max=.. mean=.. rgb=OK rgb_bytes=921600 verdict=PASS
```

Result globals available to SWD tooling are `g_cam_chipid`, `g_cam_frame_ok`,
`g_cam_min`, `g_cam_max`, `g_cam_mean`, `g_cam_verdict`, and the four RGB888
buffers `g_cam_rgb_0`, `g_cam_rgb_90`, `g_cam_rgb_180`, `g_cam_rgb_270`.

## Validation Status

Validated on EK-RA8D2 silicon on 2026-08-13:

- OV5640 SCCB chip ID reads `0x5640`.
- PCLK, HREF, VSYNC, and all eight DVP data lines show activity.
- CEU completes a 614400-byte VGA capture.
- RA8 firmware produces four exact 921600-byte RGB888 rotations in SDRAM.
- SWD dumps of all four rotations are clean, recognizable, and coordinate-exact.
- Repeated captures encode successfully as MP4 video.

The working mode follows Renesas's CEU VGA data-synchronous capture model and
uses a live sensor image; the OV5640 test pattern is disabled.

## Hardware

Use an EK-RA8D2 with the OV5640 Camera Expansion Board connected to underside
FFC port J35. Firmware drives SW4-6 through U15, so the physical switch does not
need to be moved. Keep J41 open: the DVP camera shares P405/P406 with the DA7212
codec. PB02-PB04 are also shared with the parallel graphics interface, so those
functions are mutually exclusive with this capture.

Drivers exercised: `ra8_ceu`, `ra8_gpt`, `ra8_i2c`, `ra8_pfs`, `ra8_gpio`,
`ra8_cache`, `ra8_sdramc`, and `ra8_board_uart_console`.

The unattended hardware and emulator gate is:

```sh
make hil-all APP=camera_capture
make eil-only APP=camera_capture
```

# camera_capture

Captures one live VGA frame from the OV5640 on the Camera Expansion Board
through the RA8D2 Capture Engine Unit, converts it to RGB in firmware, and
reports a plausibility verdict over the console. Host tooling dumps the buffers
over SWD.

## Hardware

Connect the OV5640 Camera Expansion Board to the underside FFC port J35. The
firmware drives the SW4-6 DVP-select override through U15 over RIIC1, so that
switch does not have to be moved by hand.

**Keep J41 open**: the DVP camera shares P405/P406 with the DA7212 codec.
PB02-PB04 are shared with the parallel graphics interface, so capture and the
parallel panel are mutually exclusive.

## Bring-up order

The order is the fragile part, not the individual register writes:

1. Generate the OV5640's roughly 24 MHz XVCLK with GPT12 on P501 and confirm the
   timer is running. The sensor has no clock of its own and will not answer SCCB
   without it.
2. Release the OV5640 reset on P709, scan SCCB, and verify chip ID `0x5640` at
   address 0x3C or 0x3D.
3. Program the VGA YUV422 sensor sequence, then sample DVP sync and data
   activity **while the pins are still GPIO inputs**. That probe is what
   distinguishes a silent sensor from a mis-configured CEU; once the pins belong
   to the CEU it is no longer observable.
4. Route VIO_D[7:0], VIO_VD, VIO_HD and VIO_CLK to the CEU and run one
   single-shot data-synchronous capture into internal SRAM.
5. Invalidate the data cache before the CPU reads the buffer, then compute
   min/max/mean statistics over it.
6. Convert UYVY to RGB888 and write the 0/90/180/270 views into four
   cache-cleaned external-SDRAM buffers.

## Rotation lives in firmware

The CEU buffer is packed Cb-Y0-Cr-Y1 after the configured input ordering and
byte swaps. The OV5640 has native mirror and vertical-flip controls but no
documented 90/270 transpose, so those two rotations are firmware-side pixel
moves after the RGB conversion. The host script adds PPM headers and nothing
else -- it never rotates -- so a wrong orientation is a firmware bug by
construction.

## Verdict

PASS requires both the CEU capture and all four RGB buffers. `g_cam_chipid`,
`g_cam_frame_ok`, `g_cam_min`, `g_cam_max`, `g_cam_mean`, `g_cam_verdict` and
the four `g_cam_rgb_*` buffers are the symbols SWD tooling reads.

The working mode follows Renesas's CEU VGA data-synchronous capture model
against a live scene; the OV5640 test pattern is deliberately disabled, so a
frame that looks plausible is a frame the optics actually produced.

Drivers exercised: `ra8_ceu`, `ra8_gpt`, `ra8_i2c`, `ra8_pfs`, `ra8_gpio`,
`ra8_cache`, `ra8_sdramc` and `ra8_board_uart_console`.

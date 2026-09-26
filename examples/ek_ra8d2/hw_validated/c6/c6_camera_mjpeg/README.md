# c6_camera_mjpeg

Camera streaming with the compression done in the sensor: the OV5640 emits JPEG,
the CEU captures the compressed byte stream straight into a caller-owned SDRAM
buffer, and NetX serves it as a persistent multipart MJPEG stream over the
ESP32-C6 link. Nothing on the RA8D2 re-encodes and nothing allocates.

It is the deliberate counterpart to `c6_camera_livestream`, which drives the
software codec backend instead. Application and network code consume the same
camera facade either way, so the pair is what proves the two backends are
interchangeable rather than parallel.

The link runs at the highest SPI rate this wiring has qualified, 10 MHz. The
next step up is deliberately not used: both control-RPC and raw-Ethernet
qualification timed out at 20 MHz, which is a property of the jumper harness
rather than of either chip.

## Bench evidence

Measured on the repository rig, and the reason this app sits under
`hw_validated/`:

- Sensor-compressed capture completes in about 188 ms per frame. No RA8D2
  software JPEG pass and no heap allocation is involved.
- A 10-second multipart transfer at 10 MHz delivered 21 complete frames:
  2.10 FPS, about 20 KiB per JPEG.
- The same application delivered 1.50 FPS at 5 MHz, which is what the rate
  change was for.

Those figures were measured when `k_c6_cam_sck_hz` was raised from 5 MHz to
10 MHz and are not a claim about the current commit; a current-pass claim needs
a dated `just hil::c6` result, as the tier README says.

The image is always credential-free. After boot it prints
`ra8_net_provision: READY v1`, accepts the same bounded runtime UART packet as
`c6_camera_livestream`, and erases credential-bearing storage immediately after
association. Received bytes are never echoed.

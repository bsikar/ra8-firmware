# c6_camera_mjpeg

Camera streaming with the compression done in the sensor: the OV5640 emits JPEG,
the CEU captures the compressed byte stream straight into a caller-owned SDRAM
buffer, and NetX serves it as a persistent multipart MJPEG stream over the
ESP32-C6 link. Nothing on the RA8D2 re-encodes and nothing allocates.

It is the deliberate counterpart to `c6_camera_livestream`, which drives the
software codec backend instead. Application and network code consume the same
camera facade either way, so the pair is what proves the two backends are
interchangeable rather than parallel.

The link runs at the highest SPI rate this wiring has qualified. The next step
up is deliberately not used: both control-RPC and raw-Ethernet qualification
timed out there, which is a property of the jumper harness rather than of either
chip.

The image is always credential-free. After boot it prints
`ra8_net_provision: READY v1`, accepts the same bounded runtime UART packet as
`c6_camera_livestream`, and erases credential-bearing storage immediately after
association. Received bytes are never echoed.

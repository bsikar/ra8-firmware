# c6_camera_livestream

Serves camera frames and microphone audio from the board over the ESP32-C6
radio. The RA8D2 runs NetX Duo, DHCP, an HTTP server, CEU capture, colour
conversion and a software JPEG encoder; the co-processor stays on its stock
esp-hosted image and does nothing but carry 802.3 frames. A browser on the bench
network gets a page that continuously reloads fresh stills, a rolling
one-second recording of MIC1 as a WAV, and a health line for automation.

`c6_camera_mjpeg` is the same idea through the other camera backend --
sensor-side JPEG with zero-copy passthrough -- behind the same facade.

## Bench routing: two peripherals contending for one switch bank

This is the only app that needs both the C6 link and the DVP camera, and the
physical DIP switches cannot be set for both. SW4 stays in the C6 position (see
the tier README) and the firmware forces the camera position of SW4-6 through
the U15 I/O expander instead.

It must do that with the *masked* board API, writing only that one expander bit.
Driving a whole U15 byte overrides the Pmod selection and disconnects the live
C6 SPI link, so the physical switch state has to stay authoritative for every
other bit.

## Things that are easy to get wrong

- The CEU, RGB and JPEG buffers live in external SDRAM, so they exist only after
  `ra8_sdramc_init()`.
- The two underside SPH0690 microphones share PDM-IF channel 2; this app takes
  MIC1 on the rising edge. Decimation happens in hardware and a bounded FIFO
  interrupt fills ping-pong SDRAM banks, so no audio thread and no heap compete
  with the JPEG and network work.
- Camera and audio responses carry timestamps from one monotonic clock, for
  offline alignment. The WAV endpoint is a rolling recording, not a synchronised
  media stream.
- Every frame response carries a `Server-Timing` header with capture,
  conversion and encode latency, so a performance regression is readable from
  browser devtools instead of being inferred from the visible frame rate.
- The bench network is not routable from the developer LAN; reaching the page
  from a workstation needs a forwarded port.

Wi-Fi credentials come from the environment or the gitignored
`coprocessor/esp32c6/wifi.env`, never from the tree. They are compiled in, so
any image built with them is a secret -- do not leave one lying in a build
directory.

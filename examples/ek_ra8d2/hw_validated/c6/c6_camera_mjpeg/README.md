# C6 Camera MJPEG

Hardware-accelerated camera streaming for EK-RA8D2. The OV5640 performs JPEG
compression, the RA8D2 CEU captures the compressed byte stream directly into a
caller-owned SDRAM buffer, and NetX serves a persistent multipart MJPEG stream
over the ESP32-C6 Wi-Fi link.

This example intentionally complements `c6_camera_livestream`: that validated
baseline uses the interchangeable software codec backend, while this example
uses the sensor-JPEG source plus zero-copy JPEG passthrough. Application and
network code consume the same camera facade either way.

Build with:

```sh
make -C examples/ek_ra8d2/hw_validated/c6/c6_camera_mjpeg
```

For a plain build-and-flash without the full HTTP verifier:

```sh
make hil-flash APP=c6_camera_mjpeg  # repository rig / bench Pi
make flash-c6_camera_mjpeg          # J-Link attached to this machine
```

Run the complete registered C6 HIL proof with:

```sh
make hil-c6 APP=c6_camera_mjpeg
```

After the firmware prints its DHCP address, expose it to the development Mac
and open `http://127.0.0.1:8080/`:

```sh
make hil-camera-tunnel IP=10.0.40.102
```

The dedicated HIL mode cold-starts the C6, proves the SPI link, captures and
decodes two changing 640x480 stills, validates a one-second microphone WAV,
and extracts two complete changing frames from the multipart MJPEG stream.
Bench capture completes in about 188 ms per sensor-compressed frame; no RA8D2
software JPEG pass or heap allocation is used.

The camera server uses the highest SPI rate qualified on the repository rig:
10 MHz. A 10-second multipart transfer delivered 21 complete frames (2.10 FPS,
about 20 KiB per JPEG). The same application delivered 1.50 FPS at 5 MHz;
20 MHz is intentionally not used because both control-RPC and raw-Ethernet
qualification timed out at that rate on this wiring.

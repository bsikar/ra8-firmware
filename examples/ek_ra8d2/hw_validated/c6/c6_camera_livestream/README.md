# ESP32-C6 camera livestream

This application combines the bench-proven OV5640/CEU capture and raw
`c6_wifi_join` ESP32-C6 path. The ESP32-C6 stays on the pinned, unmodified
esp-hosted firmware; the RA8D2 runs NetX Duo, DHCP, the HTTP server, capture,
colour conversion and JPEG encoding.

## Browser interface

After boot, SCI8 prints:

```text
c6_cam: PASS Wi-Fi and DHCP ip=192.168.x.y
c6_cam: open http://192.168.x.y/
c6_cam: PASS HTTP camera server listening
```

Open the printed URL. `/` serves a no-dependency page that continuously loads
fresh `/frame.jpg` responses. `/health` returns
`PASS c6 camera livestream` for automation.

The CEU captures the validated 640x480 packed UYVY frame. The server samples
every other row and pixel into 320x240 RGB888 and encodes quality-65 baseline
JPEG. The CEU, RGB and JPEG buffers live in external SDRAM after
`ra8_sdramc_init()`.

## Hardware switches

The C6 and DVP camera need a combined routing that neither standalone example
uses:

- Physical SW4 must be `1=OFF, 2=OFF, 3=ON, 4=OFF`: OFF/OFF selects Pmod1
  SPI, SW4-3 frees the bus from Octo-SPI, and SW4-4 keeps Arduino/mikroBUS off.
- Firmware drives only U15 bit 5 low, forcing SW4-6 ON for DVP while leaving
  every other U15 pin as an input so the proven physical C6 switch state stays
  authoritative.
- The C6 must be powered over its USB connection and wired to J26 as documented
  in `examples/ek_ra8d2/hw_validated/c6/README.md`.

Driving a whole U15 byte is unsafe here: it can override the Pmod selection and
disconnect the live C6 SPI link. The combined app therefore uses the masked
board API and changes SW4-6 only. The link runs at the probe-proven 1 MHz cold-
start rate.

## Build

Credentials are loaded from `RA8_C6_WIFI_SSID` / `RA8_C6_WIFI_PSK` or the
gitignored `coprocessor/esp32c6/wifi.env`:

```sh
make c6_camera_livestream
```

The build directory contains a generated credential header, and the firmware
image necessarily embeds those credentials. Treat build artifacts as sensitive
and run `make clean` after manual bench builds. The HIL script uses a temporary
build directory and removes it automatically.

The standalone equivalent is:

```sh
make -C examples/ek_ra8d2/hw_validated/c6/c6_camera_livestream
```

## Physical validation

The end-to-end script builds with bench credentials, cold-cycles the OpenBao-
backed Tapo outlet, waits for all USB identities, and requires the validated C6
SPI probe to pass before flashing the combined app. It then waits for the
DHCP/server banner and runs HTTP probes from the bench Pi. It checks the health
body, retains two independently captured frame responses under
`/tmp/ra8-camera-livestream`, decodes them, requires 320x240 dimensions, and
requires their encoded bytes to differ:

```sh
bash scripts/hil/camera_livestream.sh
```

On 2026-08-13 this workflow passed on the physical rig at `10.0.40.102`; both
retained JPEGs decoded as changing 320x240 room images without tearing.

The app is registered in the C6 HIL lane with
`HIL_MODE=c6_camera_livestream`, so it also runs through:

```sh
make hil-c6 APP=c6_camera_livestream
```

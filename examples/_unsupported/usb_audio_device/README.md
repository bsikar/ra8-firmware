# usb_audio_device

USB Audio Class 1.0 device-mode smoke test for the EK-RA8D2. Brings up
the USB-FS controller in device mode via the hand-written `ra_usb` +
`ra_usb_paud` stack and exposes the board as a UAC1 microphone /
headset device. The firmware feeds a precomputed 1 kHz sine wave
(48-sample stereo LUT, exactly one cycle at 48 kHz / 16-bit / stereo)
into the iso-IN endpoint every USB-FS frame so the host can render the
test tone.

This app uses the **on-board USB-FS receptacle** on the EK-RA8D2,
**not** the USB-HS receptacle.

## Status: hardware bring-up

Pin set is fixed by the EK-RA8D2 board -- USB-FS lines come out of the
chip at P407/P500/P814/P815 with PSEL = `k_ra_psel_usb_fs` (0x13).

| Net           | Pin    | PFS PSEL                | Direction                        |
|---------------|--------|-------------------------|----------------------------------|
| USB_FS_VBUS   | P4_07  | k_ra_psel_usb_fs (0x13) | VBUS sense (peripheral input).   |
| USB_FS_VBUSEN | P5_00  | k_ra_psel_usb_fs (0x13) | VBUS-enable drive (output).      |
| USB_FS_DP     | P8_14  | k_ra_psel_usb_fs (0x13) | D+ data line (analog buffer).    |
| USB_FS_DM     | P8_15  | k_ra_psel_usb_fs (0x13) | D- data line (analog buffer).    |

## Audio format

USB Audio 1.0 sec 2.2.5 "Format Type Descriptor" -- Type-I PCM.

- 48000 Hz sample rate.
- 2 channels (stereo).
- 16-bit sub-frame.
- Iso-IN max-packet 192 bytes per FS frame (`48 * 2 * 2`).
- Volume Q8.8 dB initialised to 0 dB (full scale, no attenuation).

The 48-sample sine LUT amplitude is `0x4000` (half full-scale) so a
naive host downstream-mixer cannot clip.

## Test on Linux

```sh
lsusb
# Bus 001 Device NN: ID 1209:000A ...
aplay -l
# card N: ... [EK-RA8D2 Audio], device 0: USB Audio [USB Audio]
arecord -D plughw:N,0 -f S16_LE -c 2 -r 48000 -d 5 sine.wav
# Should record a 1 kHz tone for 5 seconds.
aplay sine.wav
```

If the device shows up under a different VID/PID, the chapter-9 path in
`libs/ra_hal/src/ra_usb.c` still needs the audio descriptor table wired
up; the README documents the design intent.

## Test on macOS

After flashing, open `System Settings -> Sound -> Input` and select the
EK-RA8D2 device from the input list. The level meter should bounce in
time with the 1 kHz tone. Audio MIDI Setup (`/Applications/Utilities/`)
shows the negotiated sample rate / channel layout.

## Build + flash

```sh
make usb_audio_device
make -C examples/usb_audio_device flash
```

## SCI8 logs

The on-board J-Link OB CDC bridge (PD_02 / PD_03 -- SCI8) prints
`audio: <N> frames sent` once every 1000 frames so an attached terminal
can confirm the iso-IN feed is running. Connect at 115200 8N1.

## What the firmware does

1. `ra_cgc_init()` -- XTAL + PLL1 -> CPUCLK0 = 1 GHz, PCLKA = 125 MHz.
2. `ra_time_init(cpuclk0_hz)` -- SysTick for `ra_delay_ms`.
3. SCI8 + LED1 + USB-FS pin-mux + USB controller bring-up via
   `ra_nsc_usb_init`.
4. `ra_usb_paud_init(k_ra_usb_speed_fs)` -- iso-IN PIPE1 / iso-OUT
   PIPE2 at the FS default 192-byte max-packet.
5. `ra_usb_paud_set_format` (48 kHz / 2 ch / 16-bit) and
   `ra_usb_paud_set_volume(0)` (0 dB).
6. `ra_nsc_usb_attach(k_ra_usb_speed_fs, true)` -- raise D+ pull-up so
   the host begins enumeration.
7. Loop: feed the 192-byte sine LUT into the iso-IN endpoint every
   1 ms, log every 1000 frames, toggle LED1 per log line.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED1 init/toggle (P600 per EK-RA8D2
v1 UM Table 24 p 31). USB-FS pin set (P407 / P500 / P814 / P815) is
the only routing the chip exposes for the on-board J11 Type-C USB-FS
receptacle (UM Table 22 "USB Full Speed Port Pin Assignments" p 30);
main.c programs this pin set directly via `ra_pfs_route_peripheral`.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 22 p 30 + Table 24 p 31, USB Audio 1.0 spec sec
2.2.5, and HUM (R01UH1065EJ0130) Ch "USBFS".

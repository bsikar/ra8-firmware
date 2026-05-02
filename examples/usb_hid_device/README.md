# usb_hid_device

USB HID boot-protocol mouse smoke test for the EK-RA8D2 USB-FS port.

After flash, the EK-RA8D2 enumerates as a 3-button + X/Y mouse and
jiggles the host cursor in a 4-pixel right/down/left/up square at 1 Hz.
LED1 (P6_00) toggles per send.

## Build

```
make build
make flash
```

## Verify (Linux)

```
lsusb                # look for "Brighton Sikarskie EK-RA8D2 HID Mouse"
xinput list          # the mouse appears as a slave pointer
```

The cursor should walk in a 4-pixel square once per second on whatever
desktop has focus.

## Verify (macOS)

```
system_profiler SPUSBDataType | grep -A6 RA8D2
```

The cursor moves on screen automatically; no driver install is needed
(the OS uses its built-in boot-protocol mouse driver).

## VID / PID

VID = 0x1209 (pid.codes free-for-experiments range), PID locally
chosen. Bench use only -- do not ship hardware that leaves the bench
with these IDs.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED1 init/toggle (P600 per EK-RA8D2
v1 UM Table 24 p 31). USB-FS pin set (P407 / P500 / P814 / P815) is
the only routing the chip exposes for the on-board J11 Type-C USB-FS
receptacle (UM Table 22 p 30).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 22 p 30 + Table 24 p 31, USB HID 1.11 Boot Mouse
profile, and HUM (R01UH1065EJ0130) Ch "USBFS".

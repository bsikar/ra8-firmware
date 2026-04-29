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

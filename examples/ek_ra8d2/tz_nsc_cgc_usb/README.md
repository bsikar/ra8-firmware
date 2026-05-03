# tz_nsc_cgc_usb

TrustZone-on USB-FS CDC ACM echo demo that proves the NSC-veneer fix
for the "CGC writes from NS context fail silently" failure mode.

## Background

On the RA8D2 the Clock Generation Circuit (CGC) registers --
SCKDIVCR, USBCKCR, USBCKDIVCR, the PLL2 control block, and friends --
live inside the System Control region, which the IDAU pins as Secure
permanently. With TrustZone enabled and an application running in
the Non-Secure world, a direct call to `ra_cgc_pll2_enable()` or
`ra_cgc_usbfs_clock_enable()` lands on memory the CPU is not allowed
to write. The store completes architecturally but is dropped at the
bus -- no fault, no log, no diagnostic. The USB-FS controller then
sees the reset-default 0 in USBCKCR (USBCKSEL = HOCO ~20 MHz), the
SIE never bus-resets a connected device, the host never enumerates
the device, and the only symptom is `dmesg` reporting "device
not accepting address".

The sibling app `tz_secure_only_usb` worked around the failure by
turning TrustZone off (everything runs Secure, all CGC writes
land). This app is the other path: keep TrustZone on but route every
CGC operation through a Non-Secure Callable (NSC) veneer that lives
in the Secure world.

## What changed

| Layer                 | File                                                  | Change                                                                                                                                          |
| --------------------- | ----------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| NSC header            | `libs/ra_nsc/inc/ra_nsc_cgc.h`                        | New: declares `ra_nsc_cgc_pll2_enable`, `ra_nsc_cgc_usbfs_clock_enable`, `ra_nsc_cgc_get_clock_hz`.                                             |
| NSC source            | `libs/ra_nsc/src/ra_nsc_cgc.c`                        | New: implements the veneers as `cmse_nonsecure_entry` thunks that forward to the underlying `ra_cgc_*` API.                                     |
| Demo CMake            | `examples/ek_ra8d2/tz_nsc_cgc_usb/CMakeLists.txt`     | Forces `RA_TRUSTZONE_ENABLE=ON` and links the NSC sources.                                                                                      |
| Demo `main.c`         | `examples/ek_ra8d2/tz_nsc_cgc_usb/main.c`             | Replaces direct `ra_cgc_pll2_enable` / `ra_cgc_usbfs_clock_enable` / `ra_cgc_get_clock_hz` calls with their `ra_nsc_cgc_*` veneers.             |
| Demo `system_init.c`  | `examples/ek_ra8d2/tz_nsc_cgc_usb/system_init.c`      | Calls `ra_trustzone_init()` so the SAU is live before any NS code runs.                                                                         |
| Linker script         | `examples/ek_ra8d2/tz_nsc_cgc_usb/linker_script.ld`   | Already provides a `.gnu.sgstubs` placement at the end of `.text` for the secure-gateway veneers `gcc -mcmse` emits.                            |

## Verifying the fix

1. `make tz_nsc_cgc_usb` (top level) or `make` from the demo dir.
2. Flash via `make flash` from the demo dir.
3. The board enumerates as `/dev/cu.usbmodem*` on macOS / `/dev/ttyACM*` on Linux.
4. Open the serial port at any baud and type characters. Each byte
   echoes back. LED1 toggles per echoed byte.

If CGC writes were silently dropping (the failure mode this demo
proves is fixed), USBCKCR would stay at 0 and the device would
never enumerate. A successful enumeration means the NSC veneer
correctly trapped into the Secure world and the CGC writes landed.

## License

MIT, Copyright (c) 2026 Brighton Sikarskie.

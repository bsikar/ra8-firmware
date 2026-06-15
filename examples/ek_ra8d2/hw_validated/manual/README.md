# examples/ek_ra8d2/hw_validated/manual/

Apps here are hardware-confirmed but cannot be tested automatically by HIL
CI because they require either:
- A physical interaction (button press) that CI cannot trigger, or
- A peripheral not present on the HIL bench (I2C controller, RTT viewer), or
- Visual confirmation on the 7-inch LCD that no programmatic check can
  substitute for.

HIL CI builds these but skips the run/verify step.  Manual sign-off is
required before promoting an app out of this directory.

To build: `make <appname>` from the repo root.

## Apps and blocking reason

| App | Why CI cannot auto-verify |
|-----|--------------------------|
| display_pal_animation | LCD render output -- requires human visual confirmation of animated frames |
| lcd_color_cycle | LCD render output -- requires human visual confirmation of color cycling |
| lcd_draw_x | LCD render output -- requires human visual confirmation of drawn 'X' |
| usb_host_file_ops | Needs a physical USB thumb drive in either USB jack; suite mutates a real removable medium |
| usb_cdc_echo | USB-FS CDC-ACM device; needs an external USB host to open the VCOM and echo |
| usb_hid_device | USB-FS HID boot-mouse device; needs an external USB host to observe reports |
| usb_msc_device | USB-FS MSC RAM-disk; needs an external USB host to mount the volume |
| usb_msc_mram | USB-FS MSC view of the MRAM; needs an external USB host to mount/read |
| usb_msc_mram_hs | USB-HS MSC view of the MRAM; needs an external USB host to mount/read |
| threadx_usbx_cdc_demo | USB-FS CDC-ACM echo (ThreadX/USBX); needs an external USB host |
| tz_secure_only_usb_fs | Secure-only USB-FS CDC echo; needs an external USB host on J11 |
| tz_secure_only_usb_hs | Secure-only USB-HS CDC echo; needs an external USB host on J7 |

The eight device-mode USB apps above were moved here from `hw_validated/hil/`
once the two USB jacks were cabled together for the board-only self-loop
self-tests (`hw_validated/hil/usb_selftest_*`): with the loop fitted, a
device-mode app's port answers the board's own host, so verifying it now needs a
separate USB host (unplug the loop, plug into a PC) -- a manual step. The
self-loop self-tests cover the same CDC / HID / MSC classes automatically.

All other manual-category apps were relocated to `hw_pending/` on 2026-05-19
because they had not yet been hardware-validated by the author. Their
HIL-ability assessments live in `hw_pending/<app>/README.md`.

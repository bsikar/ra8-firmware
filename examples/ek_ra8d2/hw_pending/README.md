# examples/ek_ra8d2/hw_pending/

Apps here compile and pass all CI gates but have **not yet been confirmed
working on hardware** or have a documented gap blocking sign-off.
These are the apps to work through next when bench time is available.

To build: `make <appname>` from the repo root, e.g. `make lcd_demo`.
Move an app to `hw_validated/` (with a `git mv`) once it passes a hardware
smoke-test and the gap below is resolved.

## Apps and blocking reasons

| App | Blocking reason |
|-----|-----------------|
| ereader | Large multi-component app (ThreadX + FileX + GUIX + ra_epub). Reflow engine, GLCDC panel wiring, and touch input are deferred. Needs full integration test on bench. |
| lcd_demo | GLCDC pin mappings marked TODO -- need cross-check against EK-RA8D2 v1 board schematic before flashing. |
| ra_bootloader | A/B bank-switch state machine is a documented stub. Needs real OTA path wired up. |
| threadx_guix_demo | Depends on the same unverified GLCDC pin table as lcd_demo. Unblock lcd_demo first. |
| threadx_ota_demo | Network download path is stubbed. Needs real HTTP client or TFTP path before hardware test. |
| tz_secure_only_usb_hs | USB HS (J12) bring-up hardware-blocked: chip reaches USB Address state but stalls after SET_ADDRESS. Needs USB analyzer to diagnose. |
| usb_host_cdc_echo | Depends on USB HS (J12) -- blocked by same issue as tz_secure_only_usb_hs. |
| usb_host_keyboard | Depends on USB HS (J12) -- blocked by same issue as tz_secure_only_usb_hs. |
| usb_host_msc_browse | Depends on USB HS (J12) -- blocked by same issue as tz_secure_only_usb_hs. |

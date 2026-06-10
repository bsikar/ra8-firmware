# examples/ek_ra8d2/hw_pending/

Apps here compile and pass all CI gates but have **not yet been confirmed
working on hardware end-to-end** or have a documented gap blocking
sign-off. These are the apps to work through next when bench time is
available.

To build: `make <appname>` from the repo root, e.g. `make dac_waveform`.
Move an app to `hw_validated/hil/` (with a `git mv`) once it passes a
hardware HIL probe AND the gap below is resolved.

## Apps and blocking reasons (current as of 2026-05-19)

### Blocked by physical hardware on the bench

| App | Blocking reason |
|-----|-----------------|
| acmphs_compare | Analog comparator output depends on stim voltage on IVCMP / IVREF pins; bare EVM floats. Needs known reference wired in. |
| dac_b_demo | DAC output goes to an analog pin; needs scope / ADC ground-truth to verify the waveform. |
| dac_waveform | Same as dac_b_demo. |
| gpio_input_demo | SW1 user button drives the only observable signal; no Pi GPIO is wired to P009 on the HIL bench. |
| gpt_capture_input | Needs external pulse train on the capture pin; no Pi-side stim wiring. |
| icu_extint_demo | Same as gpio_input_demo -- needs Pi GPIO wired to SW1 (P009). |
| i3c_i2c_peripheral_demo | I2C peripheral mode needs an external controller to talk to; bench has no controller wired up. |
| imu_lsm6dso_demo | Needs LSM6DSO IMU on the I2C bus; out-of-scope for this user. |
| i2c_loopback | U15 expander chip on the bench is unresponsive under all tested SW4-5 configs. |
| kint_demo | Same as gpio_input_demo / icu_extint_demo -- SW1 stim needed. |
| threadx_filex_demo | Needs SD card (FileX over SDHI); out-of-scope for this user. |
| tz_secure_only_sd | Needs SD card; out-of-scope. |
| usb_host_cdc_echo | Needs a USB-CDC peripheral wired into J7 for the chip to enumerate; needs USB Host stack debugging. |
| usb_host_keyboard | Needs USB keyboard plus the same USB Host work. |
| usb_host_msc_browse | Needs USB mass-storage device plus USB Host work. |

### Blocked by firmware-side gaps (substantial work)

| App | Blocking reason |
|-----|-----------------|
| cpu1_pingpong | IPCSAR is secure-only-writable from CPU0's NS state; needs TrustZone bring-up so the Secure veneer can flip the bits before CPU1 release. See app README. |
| threadx_netx_tcp_echo | HAL's GWCA stub doesn't wire descriptor list addresses (GWDCBAC0/1 + LINKFIX table per HUM Ch 34.5.1.3). Multi-hour port. FSP reference: `r_layer3_switch.c` at github.com/renesas/fsp under `ra/fsp/src/r_layer3_switch/`. |
| tz_nsc_cgc_usb | NSC veneer path to ra_cgc_pll2_enable returns non-OK from NS context (init step halts at 1). NSC bridge wiring needs investigation. |

# examples/ek_ra8d2/hw_validated/hil/

Apps here are hardware-confirmed AND HIL-testable: a CI job on the
self-hosted Pi runner can flash the binary, exercise the firmware
unattended, and assert pass/fail without a human in the loop.

The HIL transport varies by app -- some scrape SCI8 UART text on
`/dev/ttyACM0`, some echo packets over USB CDC, CAN, or Ethernet, some
toggle GPIOs read back through the J-Link or a logic analyser. They
all share the property that **the test runs unattended** and the
result is observable in CI.

Apps that genuinely require a human (push a button, look at the LCD,
listen to audio) live in [`../manual/`](../manual/).

Apps that were HIL-tiered but did **not** pass the most recent bench run
(blocked by a bench-config change, an unseated SD card, absent external
hardware, or a state the harness cannot probe) have moved to
[`../../hil_needs_revalidation/`](../../hil_needs_revalidation/); that tier's
README records the real reason and re-validation path for each. Only
currently-green apps remain here.

## Build

`make <appname>` from the repo root, e.g. `make blink`, `make
uart_hello`, `make usb_selftest_soak`. The top-level Makefile
auto-discovers everything in this directory; no enumeration here is
required to add a new app.

## How each app is verified in CI

### UART-scrape (output on SCI8 -> `/dev/ttyACM0`)

`scripts/hil/all.sh` flashes the app, opens the board console (resolved by
device identity, never by ttyACM number), and asserts an expected string
appears within the per-app timeout its `hil.conf` declares.

| App | Expected UART string | Timeout |
|-----|---------------------|---------|
| adc_b_demo | `adc: raw=` | 10 s |
| agt_periodic | `agt: tick` | 10 s |
| crc_demo | `crc: hw=` | 10 s |
| crypto_aes_demo | `aes: round-trip OK` | 15 s |
| dma_memcopy_demo | `dma: copied` | 10 s |
| elc_event_demo | `elc: en=` | 10 s |
| eth_loopback | `etha: loopback ok` | 20 s |
| iwdt_demo | `iwdt: refresh in window` | 15 s |
| sdram_benchmark | `sdram: w=` | 20 s |
| threadx_fs_demo | `ospi FAT roundtrip ok` | 20 s |
| threadx_ipc_demo | `[ipc_demo]` | 15 s |
| timer_capture_demo | `gpt: period=` | 15 s |
| tz_secure_only_sd | `sd: roundtrip ok` | 20 s |
| uart_hello | `hello, ra8d2!` | 10 s |
| ulpt_demo | `ulpt: wake` | 15 s |
| watchdog_demo | `wdt: boot reason=power_on` | 15 s |

### USB self-loop self-tests (board-only, no external host)

The board's two USB jacks (J7 HS, J11 FS) are cabled **to each other** and one
image runs BOTH roles, so the whole USB data path is validated on-chip with no
PC -- the preferred HIL transport (it exercises the host stack AND the device
stack together). `scripts/hil/run_local.sh <app>` flashes, then scrapes the pass
banner on SCI8. Device-mode apps that need a separate USB host to verify live in
[`../manual/`](../manual/).

| App | Roles (J7 / J11) | Pass banner |
|-----|------------------|-------------|
| usb_selftest_hs_host | HS host / FS device (MSC MRAM) | `USB SELFTEST CONFIG A PASS` |
| usb_selftest_fs_host | FS host / HS device (MSC MRAM) | `USB SELFTEST CONFIG B PASS` |
| usb_selftest_cdc | HS host / FS device (CDC-ACM echo) | `USB SELFTEST CDC-ECHO PASS` |
| usb_selftest_hid | HS host / FS device (HID reports) | `USB SELFTEST HID PASS` |
| usb_selftest_microsd | HS host / FS device (Pmod2 microSD) | `USB SELFTEST MICROSD PASS` |
| usb_selftest_mlun | HS host / FS device (2-LUN MSC) | `USB SELFTEST MULTI-LUN PASS` |
| usb_selftest_ospi | HS host / FS device (OSPI drive RO) | `USB SELFTEST OSPI PASS` |
| usb_selftest_ospi_rw | HS host / FS device (OSPI writable) | `USB SELFTEST WRITABLE-OSPI PASS` |
| usb_selftest_soak | HS host / FS device (endurance + benchmark) | `USB SELFTEST SOAK PASS` |
| usb_host_msc_browse | HS host browses the emulated FS MSC device's FAT | `USB HOST MSC BROWSE PASS` |
| usb_host_keyboard | HS host decodes a emulated FS boot-keyboard's keycodes | `USB HOST KEYBOARD PASS` |
| dfu_selftest_hs_host | HS host / FS device (DFU programs + verifies real MRAM) | `USB SELFTEST DFU CONFIG A PASS` |
| dfu_selftest_fs_host | FS host / HS device (DFU programs + verifies real MRAM) | `USB SELFTEST DFU CONFIG B PASS` |
| dfu_selftest_boot | HS host / FS device (DFU-flash + commit a bootable Slot A; the board is its own dfu-util) | `USB SELFTEST DFU-BOOT COMMIT PASS` |

### Self-validating echoes (CAN / Ethernet / GPIO)

These apps produce no UART text but loop their own output back through
an internal peripheral so the firmware itself asserts correctness via
LED state or J-Link memory reads. Wiring these into HIL CI is
straightforward (read LED GPIO over the J-Link, or open a socket /
CAN driver on the Pi) and tracked in [`docs/HIL.md`](../../../../../docs/HIL.md).

| App | How CI verifies (or "could verify") |
|-----|--------------------------------------|
| acmphs_compare | LED1/LED2 toggle based on comparator output |
| blink | LED1 blinks at 1 Hz |
| blink_hal | LED1/2/3 blink at 1 Hz via board HAL |
| can_classic_loopback | LED1 toggles on each CAN 2.0B round-trip |
| canfd_filter_demo | LED1 toggles on accepted frames, LED2 on filtered |
| canfd_loopback | LED1 toggles on each CAN-FD round-trip |
| clock_check | LEDs toggle at exactly 1 Hz |
| cpu1_pingpong | LED1/LED2 toggle as M85/M33 ping-pong messages |
| dac_b_demo | DAC0 output ramps 0-3.3 V |
| dac_waveform | DAC0 triangle wave ~8 Hz |
| dfu_bootloader | alive: boots + runs its A/B boot decision (copy-to-run a valid slot into the SRAM run window, or the USB-FS DFU device) without faulting -- PC in code, CycleCnt advancing, CFSR/HFSR clean. DFU program/commit validated by `dfu_selftest_boot`; copy-to-run from either slot by `dfu_copy_to_run` + the J-Link sentinel in its README |
| dfu_copy_to_run | alive: copy-to-run proof (#97) -- embeds one image, hands it to `ra8_dfu_launch` (the bootloader's launcher), which copies it to the SRAM run base and branches there; PASS == PC in the SRAM run window (0x22020000+), reachable only by copying-to-run |
| doc_demo | LED1 toggles on match, LED2 latches on diverge |
| gpio_input_demo | LED1 mirrors SW1 state |
| gpt_capture_input | LED toggles on SW1 press; captures period |
| gpt_pwm_demo | LED breathes via GPT PWM duty cycle |
| sd_font_render | `g_sfr_heartbeat` advances (J-Link memprobe, after a 12 s render dwell) only once the SD-font render reaches its idle loop -- proves the whole microSD -> ra8_sdfont (self-provision) -> ra8_reflow -> framebuffer path. Any failure stage parks in `sfr_panic_halt` and the counter freezes |
| threadx_blink | LED1/LED2 toggle from two ThreadX threads |
| threadx_canfd_demo | LED toggles on each CAN-FD ThreadX frame |
| threadx_levelx_demo | LED1 toggles on each LevelX erase/write cycle |
| threadx_mpu_partition_demo | LED1 blinks in MPU-partitioned ThreadX thread |

Device-mode USB apps that need a separate USB host to verify (CDC / HID / MSC
device, the `tz_secure_only_usb_*` and `usb_msc_mram*` images) now live in
[`../manual/`](../manual/) -- the self-loop self-tests above cover the same
classes board-only. `tz_nsc_cgc_usb` remains in
[`../../hw_pending/`](../../hw_pending/) (the TrustZone partition gap, #60).

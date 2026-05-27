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

## Build

`make <appname>` from the repo root, e.g. `make blink`, `make
uart_hello`, `make tz_secure_only_usb_hs`. The top-level Makefile
auto-discovers everything in this directory; no enumeration here is
required to add a new app.

## How each app is verified in CI

### UART-scrape (output on SCI8 -> `/dev/ttyACM0`)

`scripts/hil_suite.sh` flashes the app, opens `/dev/ttyACM0`, and
asserts an expected string appears within a per-app timeout.

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
| lpm_idle_demo | `lpm: wake_count=` | 15 s |
| power_profiler | `pp: a=` | 15 s |
| rng_demo | `trng: ` | 10 s |
| rtc_alarm | `rtc: alarm fired` | 30 s |
| sdram_benchmark | `sdram: w=` | 20 s |
| threadx_ipc_demo | `[ipc_demo]` | 15 s |
| timer_capture_demo | `gpt: period=` | 15 s |
| uart_hello | `hello, ra8d2!` | 10 s |
| ulpt_demo | `ulpt: wake` | 15 s |
| watchdog_demo | `wdt: boot reason=power_on` | 15 s |

### USB CDC echo (auto-validated via `/dev/ttyACM*`)

`scripts/hil_usb_test.sh` flashes the app, waits for the device to
enumerate as 1209:xxxx, runs correctness + streaming-throughput
benchmarks against the resulting `/dev/ttyACMx`, and tests
re-enumeration via PPPS / authorized-toggle.

| App | Controller | Connector | Throughput floor |
|-----|------------|-----------|------------------|
| tz_secure_only_usb_fs | USBFS | J11 (micro-USB) | 250 KB/s one-way |
| tz_secure_only_usb_hs | USBHS | J7 (USB-C) | 2000 KB/s one-way |

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
| doc_demo | LED1 toggles on match, LED2 latches on diverge |
| gpio_input_demo | LED1 mirrors SW1 state |
| gpt_capture_input | LED toggles on SW1 press; captures period |
| gpt_pwm_demo | LED breathes via GPT PWM duty cycle |
| mpu_partition_simple | LED2 on MemFault (expected), LED3 on no-fault |
| threadx_blink | LED1/LED2 toggle from two ThreadX threads |
| threadx_canfd_demo | LED toggles on each CAN-FD ThreadX frame |
| threadx_filex_demo | LED1 toggles on each FileX write/read pass |
| threadx_levelx_demo | LED1 toggles on each LevelX erase/write cycle |
| threadx_mpu_partition_demo | LED1 blinks in MPU-partitioned ThreadX thread |
| threadx_usbx_cdc_demo | USB CDC echo, verified like the tz_* USB apps |
| tz_nsc_cgc_usb | TrustZone NSC + USB FS CDC echo |
| usb_cdc_echo | USB FS CDC echo (older simpler variant of tz_secure_only_usb_fs) |
| usb_hid_device | USB HID report toggles host caps-lock indicator |
| usb_msc_device | USB MSC volume mounts on host |

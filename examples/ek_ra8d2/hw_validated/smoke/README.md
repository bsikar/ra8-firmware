# examples/ek_ra8d2/hw_validated/smoke/

Apps here are hardware-confirmed but produce no output on the UART -- they
use LEDs, DAC outputs, an oscilloscope probe, or require an external host
(USB host, Ethernet cable) that is not always present on the HIL bench.

HIL CI tests these by flashing, letting the app run for a few seconds, then
reconnecting via J-Link and verifying the CPU is alive (no hard fault, SWD
responsive).  This is a weaker but still meaningful regression check.

To build: `make <appname>` from the repo root.

## Apps and verification method

| App | How to confirm on hardware |
|-----|--------------------------|
| acmphs_compare | LED1/LED2 toggle based on comparator output |
| blink | LED1 blinks at 1 Hz |
| blink_hal | LED1/2/3 blink at 1 Hz via board HAL |
| can_classic_loopback | LED1 toggles on each CAN 2.0B round-trip |
| canfd_filter_demo | LED1 toggles on accepted frames, LED2 on filtered |
| canfd_loopback | LED1 toggles on each CAN-FD round-trip |
| clock_check | LEDs toggle at exactly 1 Hz (stopwatch) |
| cpu1_pingpong | LED1/LED2 toggle as M85/M33 ping-pong messages |
| dac_b_demo | DAC0 output ramps 0-3.3 V (oscilloscope) |
| dac_waveform | DAC0 triangle wave ~8 Hz (oscilloscope) |
| doc_demo | LED1 toggles on match, LED2 latches on diverge |
| ethernet_tcp_echo | Needs Ethernet cable; LED when link up |
| flash_journal | LED1 toggles on each successful NOR round-trip |
| gpio_input_demo | LED1 mirrors SW1 state |
| gpt_capture_input | LED toggles on each SW1 press; captures period |
| gpt_pwm_demo | LED breathes via GPT PWM duty cycle |
| mpu_partition_simple | LED2 on MemFault (expected), LED3 on no-fault |
| threadx_blink | LED1/LED2 toggle from two ThreadX threads |
| threadx_canfd_demo | LED toggles on each CAN-FD ThreadX frame |
| threadx_filex_demo | LED1 toggles on each FileX write/read pass |
| threadx_levelx_demo | LED1 toggles on each LevelX erase/write cycle |
| threadx_lwip_tcp_echo | Needs Ethernet cable; echoes TCP to host |
| threadx_mpu_partition_demo | LED1 blinks in MPU-partitioned ThreadX thread |
| threadx_netx_tcp_echo | Needs Ethernet cable; NetX TCP echo server |
| threadx_usbx_cdc_echo | Needs USB host on J7; CDC echo via USBX |
| tz_nsc_cgc_usb | Needs USB host on J7; TZ NSC + USB FS CDC |
| tz_secure_only_usb | Needs USB host on J7; secure-world USB FS CDC |
| usb_cdc_echo | Needs USB host on J7; USB FS CDC echo |
| usb_hid_device | Needs USB host on J7; USB FS HID device |
| usb_msc_device | Needs USB host on J7; USB FS MSC device |

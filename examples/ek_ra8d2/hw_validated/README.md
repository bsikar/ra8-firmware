# examples/ek_ra8d2/hw_validated/

Apps here have been flashed to a stock **EK-RA8D2 v1** evaluation kit and
confirmed working. CI smoke-tests and stack-usage sweeps run over this set.

To build: `make <appname>` from the repo root, e.g. `make blink`.

## Apps

| App | What it demonstrates |
|-----|----------------------|
| acmphs_compare | ACMPHS analog comparator with threshold polling |
| adc_b_demo | ADC_B software-triggered sampling with UART output |
| agt_periodic | AGT free-running 1 Hz tick |
| blink | Minimal LED blink on reset-default MOCO clock |
| blink_hal | Multi-LED blink via board HAL |
| can_classic_loopback | CAN 2.0B internal loopback TX/RX |
| canfd_filter_demo | CAN-FD acceptance filter with multiple slots |
| canfd_loopback | CAN-FD internal loopback with frame validation |
| clock_check | CGC clock bring-up and PLL lock smoke test |
| cpu1_pingpong | Dual-core CPU0/CPU1 IPC ping-pong |
| crc_demo | Hardware vs software CRC-32 cross-check |
| crypto_aes_demo | AES-128-GCM encrypt/decrypt via PSA |
| dac_b_demo | DAC_B 12-bit DC sweep |
| dac_waveform | DAC_B triangle-wave generator |
| dma_memcopy_demo | DMAC SRAM-to-SRAM copy with verification |
| doc_demo | Data Operation Circuit hardware adder cross-check |
| elc_event_demo | Event Link Controller software-triggered events |
| eth_loopback | ETHA MAC-only internal loopback |
| ethernet_tcp_echo | Hand-crafted TCP/ICMP/ARP responder |
| flash_journal | Octo-SPI flash erase/program/read round-trip |
| gpio_input_demo | SW1 polling with LED1 mirror |
| gpt_capture_input | GPT free-running counter with SW1 period capture |
| gpt_pwm_demo | GPT PWM saw-wave LED breathing |
| i2c_loopback | IIC_B bus scan on unpopulated bus (NACK expected) |
| icu_extint_demo | External interrupt capture via ICU |
| iic_b_peripheral_demo | IIC_B peripheral (slave) mode |
| iwdt_demo | Independent watchdog periodic refresh and panic |
| kint_demo | Keyboard interrupt with debounce |
| lpm_idle_demo | Low-power idle entry and wake-up |
| mpu_partition_simple | MPU partition setup without TrustZone |
| power_profiler | Power profiling with LVD monitoring |
| rng_demo | Hardware RNG entropy test |
| rtc_alarm | RTC alarm with time setting and interrupt |
| rtt_log_demo | SEGGER RTT logger initialization |
| sdram_benchmark | SDRAM bandwidth and latency measurement |
| spi_loopback | SPI internal loopback TX/RX round-trip |
| ssie_audio_loop | SSIE I2S audio loopback |
| threadx_blink | ThreadX kernel init + LED blink thread |
| threadx_canfd_demo | ThreadX with CAN-FD frame exchange |
| threadx_filex_demo | ThreadX + FileX SD card read/write |
| threadx_filex_levelx_demo | ThreadX + FileX + LevelX wear-leveling |
| threadx_ipc_demo | ThreadX inter-thread queue messaging |
| threadx_levelx_demo | LevelX flash wear-leveling with FileX |
| threadx_lwip_tcp_echo | lwIP TCP echo server |
| threadx_mpu_partition_demo | ThreadX with MPU memory isolation |
| threadx_netx_tcp_echo | NetX TCP echo responder |
| threadx_usbx_cdc_demo | ThreadX USB CDC virtual serial device |
| timer_capture_demo | GPT input capture with edge counting |
| tz_nsc_cgc_usb | TrustZone S/NS partition + CGC + USB FS |
| tz_secure_only_usb | TrustZone secure-world USB FS demo |
| uart_hello | SCI8 UART hello world over J-Link OB |
| ulpt_demo | Ultra Low-Power Timer periodic tick |
| usb_cdc_echo | USB CDC ACM echo device (full-speed) |
| usb_hid_device | USB HID keyboard device (full-speed) |
| usb_msc_device | USB mass-storage device with sector read/write |
| watchdog_demo | Watchdog refresh and timeout behavior |

# examples/ek_ra8d2/hw_validated/uart/

Apps here are hardware-confirmed AND produce output on the J-Link OB CDC
UART (SCI8, /dev/ttyACM0 on the Pi).  HIL CI tests each one by flashing,
then asserting that a specific string appears on the serial port within a
timeout.  This is the strongest automated test we can run.

To build: `make <appname>` from the repo root.

## Apps

| App | Expected UART string | Timeout |
|-----|---------------------|---------|
| adc_b_demo | `adc: raw=` | 10 s |
| agt_periodic | `agt: tick` | 10 s |
| crc_demo | `crc: hw=` | 10 s |
| crypto_aes_demo | `aes: round-trip OK` | 15 s |
| dma_memcopy_demo | `dma: copied` | 10 s |
| elc_event_demo | `elc: en=` | 10 s |
| eth_loopback | `etha: loopback ok` | 20 s |
| i2c_loopback | `iic_b: scan` | 10 s |
| icu_extint_demo | `icu: irq13` | 10 s |
| iwdt_demo | `iwdt: refresh in window` | 15 s |
| lpm_idle_demo | `lpm: wake_count=` | 15 s |
| power_profiler | `pp: a=` | 15 s |
| rng_demo | `trng: ` | 10 s |
| rtc_alarm | `rtc: alarm fired` | 30 s |
| sdram_benchmark | `sdram: w=` | 20 s |
| spi_loopback | `spi: pass` | 10 s |
| ssie_audio_loop | `ssie: loop ok` | 15 s |
| threadx_filex_levelx_demo | `fxlx` | 30 s |
| threadx_ipc_demo | `[ipc_demo]` | 15 s |
| timer_capture_demo | `gpt: period=` | 15 s |
| uart_hello | `hello, ra8d2!` | 10 s |
| ulpt_demo | `ulpt: wake` | 15 s |
| watchdog_demo | `wdt: boot reason=power_on` | 15 s |

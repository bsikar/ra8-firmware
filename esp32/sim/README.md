# sim/ -- host-side simulator + companion-IC link (roadmap)

This directory will hold the ESP32-C6 host simulator, following the RA8 tree's
`tools/ra8_emulator` philosophy: run the **real firmware** on the host with the
peripherals **modelled**, so the application and the two-chip protocol can be
exercised with zero hardware. Nothing here is implemented yet; this is the plan
the spike commits to.

## Goals

1. **Run the same `src/main.c` and HAL callers unmodified.** The simulator is
   just a third backend behind `hal/esp_hal.h`: a `drivers/sim/` implementation
   whose `esp_uart_sim_ops()` / `esp_gpio_sim_ops()` write to modelled state
   instead of MMIO. The composition root picks it under a build switch. No
   application code is `#ifdef`-ed.
2. **Model only what the firmware touches.** For the current spike that is the
   UART0 TX FIFO (bytes land on the host's stdout / a capture buffer) and the
   GPIO output latch (pin transitions logged, so the blink is observable as a
   trace). The models expose the same register-level behaviour the drivers
   assume (e.g. `TXFIFO_CNT` draining over simulated time) so the bounded spins
   terminate.
3. **Model the companion-IC link.** The C6 sits next to the RA8 as a wireless
   companion. The link (UART / SPI framed protocol, TBD) is modelled as an
   in-process channel so the RA8-side `board_sim` and this simulator can be
   run together and the cross-chip protocol driven end to end -- the north-star
   "one firmware, two chips" story tested with no radios and no boards.

## Shape (planned)

```
sim/
  main_host.c        host entry: build the sim backend, call app_main()
  model_uart.c       UART0 TX-FIFO model (stdout + capture, timed drain)
  model_gpio.c       GPIO output-latch model (transition log)
  link_companion.c   modelled C6<->RA8 channel (framed protocol endpoint)
```

`make -C esp32 sim` (today a roadmap stub) will compile these with the host
compiler (not the cross toolchain), link against the same `hal/` interface, and
run the firmware host-side.

## Why a simulator at all

The RA8 experience is that a host simulator catches real firmware bugs before
the bench (a modelled peripheral once caught a 16-of-32-byte HAL bug; a
host/target divergence turned out to be firmware UB). The C6 gets the same
safety net, and -- critically -- it lets the companion-IC protocol between the
two chips be developed and regression-tested long before both boards are wired
together.

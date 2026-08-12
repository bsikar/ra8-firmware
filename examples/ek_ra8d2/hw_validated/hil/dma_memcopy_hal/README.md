# dma_memcopy_hal

DMAC SRAM-to-SRAM 1 KB copy + verify for the bare EK-RA8D2 EVM, driven
entirely through the `ra8_dmac` HAL primitives.

This is the HAL-primitive twin of `dma_memcopy_demo`. It runs the same
1 KB block copy, but software-fires the channel with
`ra8_dmac_software_trigger()` and waits for completion with
`ra8_dmac_wait_idle()` instead of poking `DMREQ.SWREQ` / `DMSTS.ACT`
through the raw channel register window. Application code here does not
include `ra8_dmac_regs.h` and touches no DMAC MMIO directly.

Both examples are kept side by side so the raw-poke and HAL-primitive
paths can be diffed on the same bench.

Brings up SCI8 + DMAC0 channel 0. Once a second fills a 1 KB source
buffer with a deterministic pattern, programmes the DMAC for a
32-bit-wide, increment-both block transfer, software-triggers the
channel, waits for it to go idle, and verifies the destination buffer
matches. Prints `dmahal: copied 1024B match=Y` on the J-Link OB CDC
channel.

- LED1 toggles on every successful copy.
- LED2 toggles on a verification mismatch or DMAC timeout.

No external hardware required.

Build / flash:

```
make dma_memcopy_hal
make -C examples/ek_ra8d2/hw_validated/hil/dma_memcopy_hal flash
```

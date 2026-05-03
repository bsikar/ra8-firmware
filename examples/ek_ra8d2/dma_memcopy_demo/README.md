# dma_memcopy_demo

DMAC SRAM-to-SRAM 1 KB copy + verify for the bare EK-RA8D2 EVM.

Brings up SCI8 + DMAC0 channel 0. Once a second fills a 1 KB source
buffer with a deterministic pattern, programmes the DMAC for a
32-bit-wide, increment-both transfer, software-triggers the channel,
waits for `DMCRA` to drain, and verifies the destination buffer
matches. Prints `dma: copied 1024B match=Y` on the J-Link OB CDC
channel.

- LED1 toggles on every successful copy.
- LED2 toggles on a verification mismatch or DMAC timeout.

No external hardware required.

Build / flash:

```
make dma_memcopy_demo
make -C examples/ek_ra8d2/dma_memcopy_demo flash
```

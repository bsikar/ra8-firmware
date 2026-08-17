# dma_memcopy_demo

Fills a 1 KB buffer with a deterministic pattern, copies it SRAM-to-SRAM with
DMAC0 channel 0 (32-bit wide, increment both, software-triggered), waits for the
transfer count to drain, and verifies the destination once a second. LED1
toggles on a good copy, LED2 on a mismatch or a DMAC timeout.

This is the raw-register twin of `dma_memcopy_hal`: it drives `DMREQ.SWREQ` and
polls `DMSTS.ACT` through the channel register window directly. The two are kept
side by side so the raw and HAL paths can be diffed on one bench. No external
hardware.

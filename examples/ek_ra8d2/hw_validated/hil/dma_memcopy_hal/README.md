# dma_memcopy_hal

The same 1 KB SRAM-to-SRAM DMAC block copy and verify as `dma_memcopy_demo`,
driven entirely through the `ra8_dmac` HAL: `ra8_dmac_software_trigger()` and
`ra8_dmac_wait_idle()` instead of poking `DMREQ.SWREQ` and polling `DMSTS.ACT`
in the raw channel register window. Application code here does not include
`ra8_dmac_regs.h` and touches no DMAC MMIO at all.

Keeping both apps means the raw and HAL paths can be run back to back on one
bench, so a HAL regression shows up as a difference between two otherwise
identical demos rather than as a silent behaviour change. LED1 toggles on a good
copy, LED2 on a mismatch or timeout. No external hardware.

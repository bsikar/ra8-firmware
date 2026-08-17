# sdram_benchmark

Brings up the on-board 64 MB Winbond W9825G6KH SDRAM through `ra8_sdramc_init`
-- the documented PALL -> MRS -> AREF-enable sequence -- at
`k_ra8_sdram_base_addr` (0x68000000), then writes and reads back a block of
incrementing 32-bit words once a second and reports throughput in MB/s.

The throughput figure is informational; the read-back compare is the test. LED1
toggles per cycle, LED2 latches on if any word comes back wrong, and the pass
verdict prints only on a clean compare -- so a bus that is fast and wrong
cannot pass for working. The SDRAM is soldered on the board; no external memory
or wiring is involved.

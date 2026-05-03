# sdram_benchmark

External-SDRAM bring-up + 64 KB write/read benchmark for the
EK-RA8D2. Uses `ra_sdramc_init` to run the documented bring-up
sequence (PALL -> MRS -> AREF enable) for the on-board 64 MB
Winbond W9825G6KH SDRAM mapped at `k_ra_sdram_base_addr`
(0x68000000), then writes / reads a 64 KB block of incrementing
32-bit words and reports throughput in MB/s.

SCI8 (115200 8N1, J-Link OB CDC) prints

```
sdram: w=NN MB/s r=NN MB/s
sdram: w=NN MB/s r=NN MB/s
...
```

once a second. LED1 toggles per cycle; LED2 latches ON if any
read-back word mismatches.

## Build + flash

```sh
make sdram_benchmark
make -C examples/ek_ra8d2/sdram_benchmark flash
```

The SDRAM is on-board -- no external memory or wiring required.

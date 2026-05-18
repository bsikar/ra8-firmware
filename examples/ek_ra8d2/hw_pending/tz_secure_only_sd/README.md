# tz_secure_only_sd

SPI-mode SD card round-trip HIL demo for the EK-RA8D2.

## What it does

1. Bring CGC + SCI8 console + RSPI bus B up.
2. Probe a Digilent PMOD MicroSD (part 410-380) plugged into J25.
3. Run the SD SPI-mode bring-up (CMD0, CMD8, ACMD41, CMD58, CMD9, CMD16).
4. Mount the FAT volume via `ra_fs`.
5. Write a 4 KiB pseudo-random payload to `/test.txt`.
6. Read it back and compare byte-for-byte.
7. Print `sd: roundtrip ok` over SCI8 on success or `sd: FAIL @ offset N` on
   the first mismatch.

The HIL runner scrapes SCI8 for `sd: roundtrip ok` (see `hil.conf`).

## Hardware

| EK-RA8D2 pin | PMOD MicroSD signal | Net (UM Table 19 p 27) |
|--------------|---------------------|------------------------|
| Pmod2.1 P604 | CS                  | SSLB0 (GPIO output)    |
| Pmod2.2 P603 | COPI                | MOSIB (UM legacy name) |
| Pmod2.3 P602 | CIPO                | MISOB (UM legacy name) |
| Pmod2.4 P601 | SCK                 | RSPCKB                 |
| Pmod2.5 GND  | GND                 | GND                    |
| Pmod2.6 3V3  | 3V3                 | 3V3                    |

The PMOD MicroSD board carries a microSD socket. Pop in a FAT16/FAT32
formatted card before booting the firmware -- the demo does **not** format.

## Build

```
make
```

Outputs `build/tz_secure_only_sd.elf` / `.hex` / `.bin`.

## Console output (success path)

```
sd: boot
sd: card ready
sd: card=32 MB
sd: fs mounted
sd: roundtrip ok
```

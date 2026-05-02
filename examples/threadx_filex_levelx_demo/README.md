# threadx_filex_levelx_demo -- FAT-on-LevelX-on-OSPI demo (EK-RA8D2)

Demonstrates a wear-levelled FAT volume mounted on top of LevelX, on
top of the on-board EK-RA8D2 Macronix MX25LM512 octal-SPI NOR flash.
This is the storage path the e-reader's local library uses when no
SD card is present.

## Stack

```
FileX (FAT, fx_media_open)
    |
    v
fx_media_driver_ra_levelx          <-- port/levelx/lx_filex_adapter.c
    |
    v
LevelX (lx_nor_flash_sector_*)
    |
    v
lx_nor_driver_ra_xspi_initialize   <-- port/levelx/lx_nor_driver_ra_xspi.c
    |
    v
ra_xspi_flash_read / program / erase_sector
    |
    v
MX25LM512 octal-SPI NOR flash on the EK-RA8D2 board
```

## What it does

1. `ra_cgc_init` brings the chip up at 1 GHz CPUCLK0.
2. SCI8 comes up at 115200 8N1 for the `[fxlx] ...` console.
3. `ra_xspi_init(0, k_ra_xspi_lio_1s1s1s)` initialises the OSPI bus.
4. `lx_nor_flash_format` + `lx_nor_flash_open` lay down a fresh
   wear-levelled NOR partition.
5. `lx_filex_adapter_bind` wires the FileX adapter to the open
   LevelX flash.
6. `fx_media_format` + `fx_media_open` lay down a fresh FAT12 volume
   on top of the LevelX-managed sectors.
7. The demo writes `"Hello from wear-leveled FAT!"` into
   `/levelx_test.txt`, closes, reopens for read, and dumps the file
   back out over SCI8.

## Build / flash

```
make            # configure + build
make flash      # JLinkExe load
```

## Expected output

```
[fxlx] booting ThreadX + FileX-on-LevelX...
[fxlx] booting xSPI flash
[fxlx] formatting + opening LevelX partition
[fxlx] formatting + opening FAT volume on LevelX
[fxlx] wrote /levelx_test.txt: Hello from wear-leveled FAT!
[fxlx] readback: Hello from wear-leveled FAT!
[fxlx] done
```

## Notes

- LevelX reserves the topmost physical sector of every block for
  mapping metadata. Only `(physical_sectors_per_block - 1) *
  total_blocks` sectors are user-visible. The demo calls
  `lx_filex_adapter_get_total_sectors()` to learn the safe total to
  pass to `fx_media_format`.
- Sector size is 512 bytes -- the LevelX default and the FAT default,
  so the adapter dispatches one LevelX sector call per FileX request
  with no chunking.
- The first run of the demo reformats both layers; persistence across
  resets is intentionally not tested by this skeleton (each cold boot
  re-formats the partition so the demo is self-contained).
- The EK-RA8D2 v1 board's on-board Octo-SPI flash is an Infineon
  IS25LX512M-JHLE (UM Section 6.3 + Table 29 p 35), not a Macronix
  MX25LM512 -- the historical "Macronix MX25LM512" label in this
  README is a copy-paste from the FSP example; treat it as "the
  on-board Octo-SPI NOR flash" regardless of vendor part.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED init/toggle (per UM Table 24 p
31). On-board Octo-SPI flash pin set per UM Table 29 p 35.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Section 6.3 + Table 29 p 35 + Table 24 p 31, and HUM
(R01UH1065EJ0130) Octo-SPI chapter.

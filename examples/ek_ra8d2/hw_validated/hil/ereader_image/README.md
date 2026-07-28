# ereader_image

Headless **on-silicon HIL gate** for the zero-heap raster image pipeline
(`ra8_reflow_image`, #106) -- the same decode -> scale -> blit path the e-reader
uses for cover art and in-chapter `<img>` figures.

It needs no panel / SDRAM / touch / SD:

1. Decode a baked **120x90 RGB PNG** cover (`cover_fixture.h`) through
   `ra8_img_decode_blit()`, allocating only from a fixed **128 KiB SRAM bump
   arena** -- so the decode reaches no `malloc` (NASA P10 Rule 3).
2. Nearest-neighbour scale it to fit a fixed **160x120 RGB565** framebuffer in
   internal SRAM (the 4:3 cover fills the 4:3 frame -> `160x120`).
3. Fold an **FNV-1a-32** hash over the whole framebuffer and print it on the
   SCI8 J-Link OB console:

   ```
   ereader-img-hil: img 160x120 crc=________
   ```

The gate (`hil.conf`, `uart_scrape`) asserts that line. Any drift in the
stb_image decoder, the integer scale math, the RGB565 pack, or the toolchain
output changes the hash and trips the gate.

The render is deterministic (a fixed PNG, integer nearest-neighbour, a fixed
RGB565 framebuffer), so the hash is identical every boot -- and the *same* hash
the identical render produces on host and under `ra8_emulator`, doubling as a
emulator/silicon equivalence check.

## Build + run

```
make ereader_image
scripts/hil/run_local.sh ereader_image      # flash + scrape the banner
```

## Result (validated 2026-06-18, ra8_emulator + host)

```
ereader-img-hil: boot
ereader-img-hil: img 160x120 crc=BDC56EC5
```

`scripts/emu/smoke.sh ereader_image` runs the firmware ELF on the
emulated RA8D2 and scrapes the banner:

```
ereader_image   OK (uart: 'ereader-img-hil: img 160x120 crc=BDC56EC5')
```

The identical decode + scale + blit run on host (real `ra8_gfx` over an RGB565
framebuffer, same fixture, same FNV) produces the **same** `crc=BDC56EC5` --
byte-for-byte agreement between the host render and the emulated RA8D2, so the
hash is a true emulator/silicon equivalence check.

> One non-obvious fix this gate caught: stb_image otherwise compiles its global
> failure-reason / flag state as `_Thread_local` (emulated TLS), which has no
> runtime on this bare-metal target and HardFaults mid-decode. The single-TU
> build defines `STBI_NO_THREAD_LOCALS` to force plain statics.

## Updating the baseline

After an **intentional** change to the fixture or the decode/scale/blit math,
recompute the hash (run under `ra8_emulator` or on the bench) and update
`HIL_EXPECT` in `hil.conf`. The on-device banner is the source of truth.

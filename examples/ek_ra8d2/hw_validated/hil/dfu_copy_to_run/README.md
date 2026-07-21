# dfu_copy_to_run -- copy-to-run HIL proof (issue #97)

An unattended, self-contained HIL proof of the [`dfu_bootloader`](../dfu_bootloader)'s
**copy-to-run** scheme: an image linked **once** at the SRAM run base
(`k_ra8_dfu_run_base = 0x22020000`) runs from wherever it is staged, so there is
no per-slot build and no "which slot am I building for?" footgun.

## What it does

1. Embeds the copy-to-run demo payload as `payload_image.h` (generated from
   [`payload.c`](payload.c) by [`build_payload.sh`](build_payload.sh)).
2. Hands it to the shared `ra8_dfu_launch` in
   [`libs/ra8_dfu`](../../../../../libs/ra8_dfu) -- **the exact launcher the
   bootloader uses** -- which copies the image to the run base and branches there.
3. The payload then spins forever in the SRAM run window, writing a sentinel
   (`0x9710C0DE`) and an advancing heartbeat at `0x22010000`.

This exercises the real copy-to-run path (`ra8_dfu_run_target_valid` + the
copy + the VTOR/MSP/branch hand-off) on silicon. The DFU *programming* path that
fills a slot is covered separately by the `dfu_selftest_*` self-loop twins.

## How HIL verifies it

`hil.conf` is `HIL_MODE=alive`. After the hand-off the **PC lives in the SRAM
run window (`0x22020000+`)** -- a region only reachable BY copying-to-run and
branching there, since this app's own code is in MRAM. So the alive gate (PC in
code, CycleCnt advancing, CFSR/HFSR clean, not parked in a fault spinner) is a
*specific* proof that copy-to-run executed:

```
scripts/hil/run_local.sh dfu_copy_to_run
```

If the launch's run-target check ever failed, control returns and parks in
`dcr_panic_halt`, which the alive gate flags as a fault spinner -> FAIL.

Unlike the bootloader's own alive gate (which can also pass via the DFU-device
fallback when no slot is valid), this app **always** copies-to-run, so its pass
is unambiguous.

## Rebuilding the embedded payload

```sh
./build_payload.sh          # rebuilds payload.bin + regenerates payload_image.h
```

Commit the refreshed `payload_image.h` whenever `payload.c` changes. The same
`payload.bin` also backs the bootloader's two-slot bench proof -- see
[`../dfu_bootloader`](../dfu_bootloader) ("One image, either slot").

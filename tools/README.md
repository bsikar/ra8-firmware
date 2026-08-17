<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# tools/

Host programs. Nothing here is linked into firmware; several of them link the
*firmware's own* libraries so that what they produce is byte-identical to what
the board would read back. Each tool has its own README or `--help`.

## Run the firmware without a board

[`ra8_emulator`](ra8_emulator/README.md) boots an unmodified cross-compiled
`.elf` on an emulated Cortex-M over a modelled RA8D2 peripheral space, drives
the GLCDC panel, and injects touch through the real GT911 path. Because it runs
the real binary, an app renders at the resolution it was *built* for: pointing a
fixed-panel app at a different `--panel` shows the genuine mismatch rather than
a re-laid-out screen.

```sh
make emu-<app> [PANEL=<name>]
```

`scripts/emu/smoke.sh` is the regression gate over it -- every display app must
reach its main loop without faulting, and the chrome app must draw a frame with
real content.

## Build data the firmware reads

| Tool | Produces |
|---|---|
| [`mkfontimg`](mkfontimg/) | A FAT SD-card image carrying a font, written through the real `ra8_fs` -- so the on-card layout is what the firmware reads back. Feeds `ra8_emulator --sd`, or a physical card. |
| [`mkbookimg`](mkbookimg/) | A FAT32 image of compiled books, streamed through `ra8_fs` and published atomically. |
| [`exfat_mkimage`](exfat_mkimage/) | An exFAT volume built through `ra8_fs`, so a real OS can mount it and judge the on-disk names. `scripts/dev/exfat_macos_interop.sh` drives it end to end on macOS. |
| [`epub_compile`](epub_compile/) | The EPUB / CBZ to `.rabook` compile pipeline. |
| [`bake_library.py`](bake_library.py) | Bakes compiled `.rabook` blobs into an MRAM-resident C header, pre-decoding each cover to a gray8 thumbnail. |
| [`rabook_imagepack`](rabook_imagepack/) | Converts one image to a `.jof` tile atlas; `inspect` dumps any first-party container's structure and `verify` round-trips it byte for byte. |
| [`vela`](vela/README.md) | Pinned Arm Ethos-U Vela -- compiles a quantized `.tflite` into an NPU command stream at build time. Nothing from it is linked into firmware. |

## Look at what the firmware produced

| Tool | Shows |
|---|---|
| [`rabook_viewer`](rabook_viewer/) | Opens a compiled document natively: `make view FILE=<doc>` (`HEADLESS=1` dumps a PPM). |
| [`mcp`](mcp/README.md) | A zero-dependency MCP server giving an assistant live repo context. `make mcp` self-tests it; a client auto-loads it from `.mcp.json`. |

## Size a cache before shipping it

Three host benchmarks back the memory hierarchy, each with a `run` target in its
own Makefile. They drive the **real** firmware code, not a re-modelled policy,
which is what makes them evidence rather than opinion.

| Tool | Question it answered |
|---|---|
| [`cache_bench`](cache_bench/README.md) | Which Layer-2 eviction policy? (the decision record that picked SLRU) and, under `--sweep-block`, what block size the chunked `.rabook` container should use. |
| [`reader_vmem`](reader_vmem/) | Does SLRU still win when the actual `ra8_vmem` drives a real reader session? Emits a trace `cache_bench` replays. |
| [`glyph_bench`](glyph_bench/) | How many cells does the `ra8_glyph_atlas` need under a real text-render stream? |

## Sign an image

`rot_sign.py` runs the root-of-trust key ceremony and signs an image with the
trailer `ra8_rot_verify_image` demands; `rot_patch_pubkey.py` provisions the
matching public key into the verifier. Both are driven by
`scripts/secrets/rot_provision.sh` -- do not hand-run half a ceremony.

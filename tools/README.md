<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# tools/

Host programs. Nothing here is linked into firmware; several of them link the
*firmware's own* libraries so that what they produce is byte-identical to what
the board would read back. `ls tools/` is the registry, and each tool has its
own README or `--help`.

## Run the firmware without a board

[`ra8_emulator`](ra8_emulator/README.md) boots an unmodified cross-compiled
`.elf` on an emulated Cortex-M over a modelled RA8D2 peripheral space, drives
the GLCDC panel, and injects touch through the real GT911 path. Because it runs
the real binary, an app renders at the resolution it was *built* for: pointing a
fixed-panel app at a different panel shows the genuine mismatch rather than a
re-laid-out screen.

`scripts/emu/smoke.sh` is the regression gate over it -- every display app must
reach its main loop without faulting, and the chrome app must draw a frame with
real content.

## Build the data the firmware reads

The image builders write FAT, FAT32 and exFAT volumes **through `ra8_fs`**, so
the on-card layout is what the firmware reads back rather than whatever a host
utility happened to produce. That is the whole point of building them here: the
exFAT one exists so a real OS can mount the result and judge the on-disk names,
and its output feeds either the emulator or a physical card.

Beside them sit the content compilers -- EPUB and CBZ into the reader-native
container, a single image into a JOF tile atlas, a whole library baked into an
MRAM-resident header with pre-decoded cover thumbnails -- and the pinned Arm
Ethos-U Vela compiler, which lowers a quantized model into an NPU command
stream at build time. Nothing Vela emits is linked into firmware.

## Look at what the firmware produced

A native viewer opens a compiled document, or dumps a frame headlessly instead
of drawing one. An inspector dumps any first-party container's structure and
round-trips it byte for byte. An MCP server gives an assistant live repo
context.

## Size a cache before shipping it

Three host benchmarks back the memory hierarchy. Each drives the **real**
firmware code rather than a re-modelled policy, which is what makes them
evidence rather than opinion. Between them they answered which Layer-2 eviction
policy to use and what block size the chunked container should have, whether
that choice still holds when an actual reader session drives the real
virtual-memory layer, and how many cells the glyph atlas needs under a real
text-render stream.

## Sign an image

The root-of-trust key ceremony and the matching public-key provisioning are
driven by `scripts/secrets/rot_provision.sh`. Do not hand-run half a ceremony.

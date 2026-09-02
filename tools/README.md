<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# tools/

Host programs. Nothing here is linked into firmware; several of them link the
*firmware's own* libraries so that what they produce is byte-identical to what
the board would read back. `ls tools/` is the registry, and each tool has its
own README or `--help`.

## Source layout

Every compiled tool keeps implementation C/C++/Objective-C files in `src/`
and authored headers in `inc/`. A compiled tool-local `tests/` build unit has
its own `src/` and, when needed, `inc/`. CMake files, READMEs, and
integration-test drivers remain at the tool root because they describe or drive
the whole unit.

`just tools::build` discovers every `tools/*/CMakeLists.txt`; the same registry
drives `just tools::list` and `just tools::clean`. Native configure always goes
through `scripts/builders/host_cmake.sh`, which proves the selected compiler can
parse the repository's C23 surface and rejects a cache made by another source
tree or compiler. A missing compiler or emulator dependency is a hard failure
with setup guidance, never a silently skipped tool.

The deliberate exemptions are non-compiled inputs and generated artifacts:
emulator panel descriptions, viewer fixtures, Vela models and generated model
headers stay in their named data directories. Python-only tools keep their
entry modules at the tool root; they have no C header surface to place in
`inc/`. A genuinely single-file script stays a single file; it is not split or
wrapped in a synthetic build project merely to imitate a multi-TU C tool.

Dependency ownership follows the product domain, not every binary that uses a
library. Content compilers and viewers therefore consume Miniz and media
codecs from `apps/shared_libs/third_party/`; their host-tool use does not make
those dependencies platform- or tool-owned. A future
`tools/<tool>/third_party/<component>` subtree is reserved for a dependency
used exclusively by that one tool. It must be registered in the SBOM and carry
the same provenance, qualification, license, and raw-byte checkout controls as
the existing vendor roots. No current dependency qualifies.

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

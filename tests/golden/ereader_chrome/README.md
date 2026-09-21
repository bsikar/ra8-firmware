# ereader_ui chrome golden images (#84)

Pinned reference renders of the `ereader_ui` example's **chrome** (the Library
and Reading screens, issue #80), used as a regression gate.

`tools/ra8_emulator` boots the real cross-built `ereader_ui.elf` on the emulated
RA8D2 and renders the GLCDC framebuffer deterministically. Each golden is that
framebuffer for one screen, cropped to the panel region -- the emulator's debug
sidebar is removed so the golden depends only on firmware output -- and gzipped,
which flat 16-level grayscale chrome takes to very well.

| file              | screen  | how it is reached            |
|-------------------|---------|------------------------------|
| `library.ppm.gz`  | Library | initial screen               |
| `reading.ppm.gz`  | Reading | a synthetic click into the reader |

`just apps::emulator::golden` cross-builds, renders and compares;
`just apps::emulator::golden_update` regenerates after an INTENTIONAL chrome change. The same
comparison runs in CI through `scripts/emu/smoke.sh`, and a failing check writes
the actual render out for inspection. The comparison logic lives in
`scripts/gen/ereader_golden.py`. Always review the new images in the diff before
committing a regeneration.

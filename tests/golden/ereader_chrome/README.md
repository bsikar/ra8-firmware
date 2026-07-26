# ereader_ui chrome golden images (#84)

Pinned reference renders of the `ereader_ui` example's **chrome** (the Library
and Reading screens, issue #80), used as a regression gate.

`tools/ra8_emulator` boots the real cross-built `ereader_ui.elf` on the emulated
RA8D2 and renders the GLCDC framebuffer deterministically. Each golden is that
framebuffer for one screen, cropped to the panel region (board_sim's debug
sidebar is removed so the golden depends only on firmware output) and gzipped
(the flat 16-level-grayscale chrome compresses ~340x, so each file is a few KB).

| file              | screen  | how it is reached            |
|-------------------|---------|------------------------------|
| `library.ppm.gz`  | Library | initial screen               |
| `reading.ppm.gz`  | Reading | `board_sim --click 250 250`  |

## Workflow

```
make ereader-golden          # cross-build + render + compare to these goldens
make ereader-golden-update   # regenerate after an INTENTIONAL chrome change
```

The same comparison runs in CI through `scripts/sim/smoke.sh` (the
`board-sim-smoke` workflow). A failing check writes the actual render to
`/tmp/ereader_golden_out/<screen>.actual.ppm` for inspection.

The comparison logic lives in `scripts/gen/ereader_golden.py`. Regenerate the
goldens whenever the chrome is deliberately changed, and review the new images
in the diff before committing.

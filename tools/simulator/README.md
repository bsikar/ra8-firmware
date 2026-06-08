# RA8D2 UI Simulator

A config-driven companion tool for building the GUIX UI on macOS without
hardware. It **becomes the display** described by a small TOML config and runs
the shared UI (`examples/shared/bedroom_ui`) against it -- click to interact, or
render a screen headless. The same UI source flashes to the EK-RA8D2 panel.

This is a development *tool* (it lives under `tools/`, not `examples/`), so the
firmware CI gates don't touch it.

> **`sim` vs `emulate-<app>`** -- `make sim` (this tool) recompiles the GUIX UI
> *natively* on macOS: fast and clickable, ideal for UI design, but **not the
> firmware**. To boot the *real cross-compiled `.elf`* on a CPU emulator and see
> its actual GLCDC output, use `make emulate-<app>` (`tools/board_sim`).

## Run

```sh
make sim                 # from the repo root: build + run on the EK-RA8D2 panel
make sim PANEL=sample_480x272
# or directly:
cd tools/simulator
cmake -B build -S . && cmake --build build -j
./build/sim --panel panels/ek_ra8d2.toml
./build/sim --panel panels/ek_ra8d2.toml --png /tmp/screen.ppm --tab 1   # headless
```

Close the window (or Cmd-Q / Esc) to exit. Headless `--png` writes a PPM
(`sips -s format png in.ppm --out out.png` to view).

## Add a display

Drop a `.toml` in `panels/` -- no rebuild needed:

```toml
name   = "My Panel"
width  = 1280
height = 720
format = "rgb565"
ppi    = 200
```

Then `make sim PANEL=<basename>`. `width`/`height`/`format` are required;
`name`/`ppi` are optional. Flat `key = value` only (a TOML subset).

## v1 scope / next

- **v1:** any size, `format = "rgb565"`, click -> tap, headless render.
- The bundled bedroom UI is laid out for 1024x600, so it clips on smaller
  panels until the UI is made resolution-adaptive -- the *display* is already
  fully configurable.
- **Next:** drag/swipe (pointer stream -> `PEN_DRAG`), a live HUD (cursor coords
  + FPS + panel name in the title), a screenshot key, and grayscale / e-paper
  (`format = "gray4"`) rendering for the e-reader path.

# ThreadX module apps

Module apps for the RA8 boards. Each directory here is a ThreadX module
packaged as a `.ra8app` (identity, capability set, entry, stack) and built
through the cross image steps in `build.zig`.

- `txm_translate_m33/`: the translation module's shell for CPU1
  (Cortex-M33), packaged with display and storage and no network
  (RA8FW-680). Its manager example lives in
  `examples/ra8p1_foundation/txm_manager_ra8p1`, which packs the built
  `.ra8app` into MRAM, admits it under the grant, and memory-loads it.

Setup examples for the `.ra8app` build and load path land here too as
they move over from `examples/`.

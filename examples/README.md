# examples/

Per-library demos that double as the hardware-in-the-loop vehicles. An app is
usually one directory holding `main.c`, a `CMakeLists.txt` stub and a README --
the vector table, boot code and linker script come from the board layer unless
the app overrides them. Each builds by bare name from the repo root:
`make <appname>`, `make apps` to list them, `make emu-<appname>` to boot one in
the emulator without a board.

Directories sort apps by **what it takes to verify them**, not by subject:

| Tier | |
|---|---|
| [`ek_ra8d2/`](ek_ra8d2/README.md) | Everything that runs on a stock EK-RA8D2, split again by hardware sign-off status. |
| [`ra8p1_foundation/`](ra8p1_foundation/) | The RA8P1 target: a blink bring-up plus the Ethos-U NPU trio (`npu_smoke`, `npu_vela`, `npu_infer`). |
| [`_unsupported/`](_unsupported/README.md) | Needs hardware this project does not own. Compiles, never flashed by CI. |

A tier is a `git mv` away: discovery is the filesystem, and an app's build-target
name does not change when it moves.

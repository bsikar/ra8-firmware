# examples/

Per-library demos that double as the hardware-in-the-loop vehicles. Every app
keeps implementation files under `src/`, headers under `inc/`, and build,
manifest, linker, documentation, and asset files at the app root. The vector
table, boot code and linker script come from the board layer unless the app
overrides them. From the repository root, use
`just apps::build <appname>` to build one, `just apps::example::list` to list
them, and `just apps::emulator::run <appname>` to boot one without a board.

Directories sort apps by **what it takes to verify them**, not by subject:

| Tier | |
|---|---|
| [`ek_ra8d2/`](ek_ra8d2/README.md) | Everything that runs on a stock EK-RA8D2, split again by hardware sign-off status. |
| [`ra8p1_foundation/`](ra8p1_foundation/) | The RA8P1 target: a blink bring-up plus the Ethos-U NPU trio (`npu_smoke`, `npu_vela`, `npu_infer`). |
| [`_unsupported/`](_unsupported/README.md) | Needs hardware this project does not own. Compiles, never flashed by CI. |

A tier is a `git mv` away: discovery is the filesystem, and an app's build-target
name does not change when it moves.

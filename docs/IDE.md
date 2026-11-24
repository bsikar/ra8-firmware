# Developer IDE Integration

The command line and Just recipes remain authoritative. The checked-in editor
configuration selects CMake targets dynamically, so adding or renaming an app
does not require a new launch entry.

## CMake presets

`CMakePresets.json` declares matching configure and build presets:

| Preset | Purpose |
|---|---|
| `ra8d2-debug` | RA8D2 cross-build with debug information |
| `ra8d2-release` | RA8D2 cross-build with optimized debug information |
| `host-debug` | Native host apps and tests with debug information |
| `host-release` | Optimized native host apps and tests |

Both VS Code CMake Tools and CLion read these presets directly. The canonical
whole-tree language-server database is the repository-root
`compile_commands.json`; regenerate it with `just apps::compile_commands`.

## VS Code

Install the recommendations in `.vscode/extensions.json`, select a configure
preset and launch target with CMake Tools, then choose one of the checked-in
launch configurations:

- `RA8: local board (OpenOCD)` builds the selected firmware target, applies the
  pre-flash guard, and launches Cortex-Debug against a locally attached board.
- `RA8: firmware in emulator (debug emulator host)` builds the selected
  firmware and emulator, then debugs the emulator process with CodeLLDB.
- `RA8: native host target (LLDB)` builds and debugs the selected native CMake
  target.

Remote-rig J-Link debugging is owned by the separate lab automation rather than
a checked-in launch entry (`hil::remote_gdb` is retired from `.vscode/` and
this repository).

The emulator currently exposes no GDB remote stub for the emulated Cortex-M
core. The checked-in configuration therefore debugs the emulator host process,
not firmware through a fictitious `localhost:1234` endpoint.

## CLion

CLion consumes the same CMake presets without generated `.idea/workspace.xml`
or `.run` files. Those files contain user-local state and are not a stable
repository interface.

For a native host target, select `host-debug`, choose the CMake target, and use
the normal CMake Application configuration. For a local board, select
`ra8d2-debug` and configure an Embedded GDB Server entry using
`scripts/dev/openocd/ek-ra8d2.cfg` and the selected target's ELF.

Remote-rig GDB tunnel orchestration (`hil::remote_gdb` and its `just` server
wrapper) has been retired from this repository; remote HIL bench execution runs
from the lab automation rather than CLion's checked-in workflows.

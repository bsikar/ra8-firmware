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
- `RA8: remote rig (J-Link)` builds and checks the selected image, takes the
  shared bench lock, starts a J-Link GDB server on the configured rig, and
  forwards it to `127.0.0.1:2331`. Ending the debug session tears down the
  tunnel and releases the liveness-bound lock.
- `RA8: firmware in emulator (debug emulator host)` builds the selected
  firmware and emulator, then debugs the emulator process with CodeLLDB.
- `RA8: native host target (LLDB)` builds and debugs the selected native CMake
  target.

Rig host and probe settings come only from the gitignored `.env`; copy
`.env.example` and set `PI_HOST` and `JLINK_SN`. The remote configuration fails
closed if the lock or SSH connection cannot be established.

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

For the remote rig, create an **Embedded GDB Server** configuration. Unlike a
before-launch External Tool, CLion owns this server process for the complete
debug session and terminates it afterward, so the bench lock has the same
liveness boundary as the editor session:

1. Select the CMake target and its ELF as the configuration's target and
   executable. Set the debugger to `arm-none-eabi-gdb`.
2. Set the configuration's working directory to `$ProjectFileDir$`, **GDB
   Server** to `/bin/bash`, and **GDB Server args** to
   `-p "$ProjectFileDir$/scripts/dev/run_just.sh" --working-directory
   "$ProjectFileDir$" hil::remote_gdb run 2331 <app>`. Set **'target remote'
   args** to `127.0.0.1:2331`. Replace `<app>` with the catalogue selector for
   the selected target, for example `blink` or
   `ek_ra8d2::hw_validated::hil::blink`. The explicit project path and Just
   working directory keep the launch independent of CLion's process directory.
3. Set **Download executable** to **None**. The server wrapper flashes the
   selected app through the guarded HIL path after taking the bench lock and
   before opening the tunnel. A second debugger-driven download would bypass
   that image guard.
4. Use `monitor reset halt` as the reset command and a startup delay of at
   least one second. Start the configuration normally; CLion starts the
   wrapper, attaches after the tunnel is ready, and stops it when the session
   ends.

If CLion is forcibly killed and a server process survives, run
`just hil::remote_gdb stop 2331`. The authenticated local broker verifies that
its direct parent belongs to this workspace before signaling it. The wrapper
then stops its retained SSH child, while the rig-side supervisor stops only its
own unreaped J-Link child; no process-name or PID sweep is used. That sequence
tears down the tunnel, remote server, and lock.

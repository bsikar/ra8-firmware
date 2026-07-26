<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# app_shell_demo -- ra8_app chrome that launches reader / library / settings

The chrome / "shell" increment of the app framework (issue #146, **Phase 2**).
It builds on Phase 1 (`app_launch_demo`: the `ra8_app` registry + per-app vtable +
navigation back-stack) and adds the piece a home screen needs: a small
**launcher** that lists the registered apps and launches the one the user picks,
with "back" unwinding the navigation trail. **No display, no widgets** -- the
launch path is observable headlessly on `board_sim` through the ITM log.

## What it proves

Acting as the device "chrome", `main.c`:

1. Registers three first-class apps into one registry, each a real `ra8_app`
   vtable (`init` / `on_enter` / `render`(draw) / `on_input`(event) / `on_leave`
   / `deinit`(teardown)):
   - `library` (id 1) -- **core, non-removable** (`removable = false`).
   - `reader` (id 2) -- **core, non-removable** (`removable = false`).
   - `settings` (id 3) -- **optional, removable** (`removable = true`), wrapped
     in a build-time guard (`#if APP_SHELL_SETTINGS`).
2. Presents a launcher: enumerates the registry (`ra8_app_count` + `ra8_app_at`),
   logs each app as a menu entry (`[name] INFO: core|removable=<id>`), and
   launches one **by its position** via `ra8_app_nav_go_index` -- the by-index
   launcher bridge added to `ra8_app` for this increment.
3. Navigates `library` -> `reader` -> `settings` with `ra8_app_nav_go`, the focus
   lifecycle firing under every move (`on_leave` -> `on_enter`) and each outgoing
   app pushed onto the back-stack. A back-button event is routed to `reader` via
   `ra8_app_route_input` to exercise the event leg.
4. Presses "back" twice with `ra8_app_nav_back`, unwinding `settings` -> `reader`
   -> `library` so the back-stack empties and `library` is foreground again.

Each stub app's callbacks just count and log their lifecycle (no real UI). A
deterministic self-check asserts the exact call counts and back-stack depth at
every step.

## Next increment (kept decoupled)

Wiring real `ra8_widget` UIs into each app (an "app = a widget tree") is the next
increment. It is deliberately left out here so this shell stays decoupled from
`ra8_widget`'s in-flight changes: the framework only routes the lifecycle + input
+ render to the active app, and the concrete draw lives in each app's `render`
callback (a logged stub today). The framework has no unregister trigger yet, so
each app's `deinit` is wired for contract completeness but not fired this round.

## Core / uninstallable (build-time exclusion)

`settings` is optional. Configure with `-DAPP_SHELL_SETTINGS=0` to drop it from
the registry entirely -- the "core uninstallable" mechanism from #146. The
firmware still builds and runs; the app count drops from 3 to 2 and the settings
navigation leg is skipped. The core `library` and `reader` apps are always
present.

## Build + run

```sh
# Cross-build the firmware:
make app_shell_demo                  # from the repo root
# or:
cd examples/ek_ra8d2/hw_pending/app_shell_demo && make

# Headless on board_sim (ITM lines show up as `[itm] ...`):
tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_pending/app_shell_demo/build/app_shell_demo.elf
```

A passing run emits (Debug build, INFO-level logs compiled in):

```
[itm] [app_shell] INFO: apps=3
[itm] [app_shell] INFO: library enters=2
[itm] [app_shell] INFO: app_shell_demo PASS
```

(`apps=2` when built with `-DAPP_SHELL_SETTINGS=0`.) Any failure logs a
`[app_shell] INFO: FAIL ...` line instead, then the CPU parks in WFI.

## Status

`hw_pending`: validated on `board_sim` (boots, runs the launcher self-check,
emits the PASS banner). Not yet run on the EK-RA8D2 hardware -- with a J-Link
attached the same ITM lines appear in the SWV / RTT console.

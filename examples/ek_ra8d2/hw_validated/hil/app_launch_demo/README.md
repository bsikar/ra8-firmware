<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# app_launch_demo -- ra8_app registry + launch + back-stack (chrome stub)

The first runnable increment of the app framework (issue #146). It exercises
the `ra8_app` lifecycle / registry / launcher plus the navigation back-stack
(`ra8_app_nav_t`) end-to-end on the real Cortex-M85 image -- **no display, no
widgets** -- so the launch path is observable headlessly on `board_sim` through
the ITM log.

## What it proves

Acting as a tiny "chrome" / shell, `main.c`:

1. Registers two stub apps into one registry:
   - `reader` (id 1) -- a **core, non-removable** app (`removable = false`).
   - `settings` (id 2) -- an **optional, removable** app (`removable = true`),
     wrapped in a build-time guard (`#if APP_LAUNCH_SETTINGS`).
2. Launches `reader` from the chrome via `ra8_app_nav_go` (first focus, nothing
   pushed) and renders it.
3. Switches to `settings` via `ra8_app_nav_go` -- the focus lifecycle fires
   (`reader.on_leave` then `settings.on_enter`) and `reader` is pushed onto the
   back-stack.
4. Presses "back" via `ra8_app_nav_back` -- `settings` leaves, `reader`
   re-enters, and the back-stack empties.

Each stub app is a function-pointer vtable (`init` / `on_enter` / `tick` /
`render` / `on_input` / `on_leave` / `deinit`) plus the `removable` flag; the
stubs count their lifecycle calls and log them. A deterministic self-check
asserts the exact call counts and back-stack depth at every step.

## Core / uninstallable (build-time exclusion)

`settings` is optional. Configure with `-DAPP_LAUNCH_SETTINGS=0` to drop it from
the registry entirely -- the "core uninstallable" mechanism from #146. The
firmware still builds and runs; the app count drops from 2 to 1 and the
switch/back leg is skipped. The core `reader` app is always present.

## Build + run

```sh
# Cross-build the firmware:
make app_launch_demo                 # from the repo root
# or:
cd examples/ek_ra8d2/hw_pending/app_launch_demo && make

# Headless on board_sim (ITM lines show up as `[itm] ...`):
tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_pending/app_launch_demo/build/app_launch_demo.elf
```

A passing run emits:

```
[itm] [app_launch] INFO: apps=2
[itm] [app_launch] INFO: reader enters=2
[itm] [app_launch] INFO: demo PASS
```

(`apps=1` when built with `-DAPP_LAUNCH_SETTINGS=0`.) Any failure logs a
`[app_launch] INFO: FAIL ...` line instead, then the CPU parks in WFI.

## Status

`hw_pending`: validated on `board_sim` (boots, runs the self-check, emits the
PASS banner). Not yet run on the EK-RA8D2 hardware -- with a J-Link attached the
same ITM lines appear in the SWV / RTT console.

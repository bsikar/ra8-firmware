<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# app_launch_demo

Exercises the `ra8_app` registry, the per-app lifecycle vtable and the
navigation back-stack end to end on the real Cortex-M85 image (issue #146).
There is deliberately no display and no widgets, so the launch path is
observable from the log alone.

Two stub apps are registered: a core `reader` and a removable `settings`. The
demo launches one, switches to the other -- which fires `on_leave` then
`on_enter` and pushes the outgoing app -- then goes back. A deterministic
self-check asserts the exact lifecycle call counts and back-stack depth at every
step and emits a single PASS or FAIL verdict, so the run is machine-gradeable
rather than something a human has to read off a log.

`settings` sits behind the `APP_LAUNCH_SETTINGS` build guard; configuring it off
drops the app from the registry entirely and skips the switch/back leg. That is
the "core uninstallable" mechanism: a core app is one that cannot be configured
out.

`app_shell_demo` is the next increment -- the same framework plus a launcher
that enumerates the registry.

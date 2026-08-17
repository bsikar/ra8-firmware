<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# app_shell_demo

The chrome / launcher increment of the `ra8_app` framework (issue #146). It
builds on `app_launch_demo` by adding what a home screen actually needs: a
launcher that enumerates the registry and starts an app **by its position**,
with back unwinding the navigation trail. Still no display and no widgets.

Three apps are registered -- core `library` and `reader`, removable `settings`
-- each a real lifecycle vtable. The demo launches by index through the
`ra8_app_nav_go_index` bridge, navigates through all three with the focus
lifecycle firing under every move, routes a back-button event to the focused app
to exercise the input leg, then unwinds the stack to empty. A deterministic
self-check asserts the call counts and back-stack depth at every step and emits
a single PASS or FAIL verdict.

`settings` sits behind the `APP_SHELL_SETTINGS` build guard; configuring it off
drops it from the registry and skips its navigation leg.

Real `ra8_widget` trees are deliberately left out. Keeping the shell decoupled
from the widget layer is what lets each app's `render` stay a callback the
framework only routes to, rather than something the framework has to understand.
The framework has no unregister trigger yet, so each app's `deinit` is wired for
contract completeness but never fires here.

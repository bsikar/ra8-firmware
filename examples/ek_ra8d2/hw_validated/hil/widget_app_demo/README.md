# widget_app_demo

Interactive, **panel-visible** demonstration of the `ra8_widget` compositor
(#145) and the `ra8_app` framework (#146) on the live GLCDC display. Where
`widget_app` (its headless sibling) CRC-gates an off-screen framebuffer, this
app brings the GLCDC panel up so the composition is shown on `ra8_emulator`'s panel
window and is driven by the physical **SW1 / SW2** push-buttons.

```
make emu-widget_app_demo      # opens the ra8_emulator panel window
```

## What it shows

1. **App registry (#146).** Three apps -- `Library`, `Reader`, `Settings` --
   register into one `ra8_app` registry. `Settings` is `removable` and wrapped in
   a build-time guard (`#if WA_APP_SETTINGS`, default 1): defining
   `WA_APP_SETTINGS` to 0 drops it from the registry entirely (the "core
   uninstallable" pattern), and the banner then reports `apps=2`.
2. **App = a widget tree (#145).** Each app is a status bar (fixed) over per-app
   content (flex) over a tab bar (fixed), laid out by `ra8_widget_layout_stack`
   and drawn by each widget's `render` through `ra8_gfx` into the GLCDC buffer.
   The status bar and tab bar are shared chrome (they read the registry); only
   the content widget differs per app.
3. **Input routing (#145 + #146).**
   - **SW1 = previous app, SW2 = next app.** A press is delivered as a `button`
     event through `ra8_app_route_input` to the focused app, whose `on_input`
     picks the neighbour app and launches it.
   - **Tap a tab.** A touch is routed through `ra8_widget_dispatch` to the tab
     bar, which maps the hit column to an app id.
   - Switching fires the focus lifecycle (`on_leave` / `on_enter`) and
     re-composites -- visibly, on the panel.

The 512x512 RGB565 framebuffer lives in SRAM (the full 1024x600 panel needs
SDRAM, a separate task) and composites top-left over the GLCDC background plane.

## Headless gate

Before the interactive loop a deterministic self-check exercises the whole
surface and emits one banner, so the app doubles as a `ra8_emulator` regression
gate (`hil.conf`, `uart_scrape`):

```
widget-app-demo: apps=3 lib=26CE7CD0 rdr=22B7E671 route=ok flush=512x44 hint=fast PASS
```

The banner asserts: all three apps registered, the Library and Reader composites
are distinct, the focus lifecycle fired exactly once each, a synthetic touch on
the Library tab (while Reader is focused) routed through `ra8_app_route_input` ->
`ra8_widget_dispatch` and selected Library, and a status-bar-only invalidation
yields exactly the status-bar rect with the `fast` hint. Any failure prints a
`FAIL ...` banner and parks. The composite CRCs are the ra8_emulator baseline.

The pure widget/app logic (layout, dispatch, routing, damage, lifecycle) is
unit-tested with MC/DC on the host (`tests/test_ra8_widget.c`,
`tests/test_ra8_app.c`); this app runs the whole thing on the target plus the
GLCDC render path, and lets you drive it by hand.

## Status

`hw_pending`: validated in `tools/ra8_emulator` (GUI render + headless banner), not
yet exercised on a stock EVM (it shares the GLCDC layer-1 bring-up that
`glcdc_render` validates on bench).

## Build

```
make widget_app_demo          # default: Settings included (apps=3)
make -C examples/ek_ra8d2/hw_pending/widget_app_demo flash
```

To exclude the optional `Settings` app, define `WA_APP_SETTINGS` to 0 (edit the
guard in `main.c` or add `-DWA_APP_SETTINGS=0` to the app's compile flags); the
registry then holds two apps and the banner reports `apps=2`.

# widget_app_demo

The panel-visible counterpart to `widget_app`: where that one CRC-gates an
off-screen framebuffer headlessly, this brings the GLCDC panel up so the
composition is actually shown, and drives it from the physical SW1 / SW2
push-buttons.

**App registry (#146).** Three apps -- Library, Reader, Settings -- register
into one `ra8_app` registry. Settings is `removable` and sits behind a
build-time guard (`WA_APP_SETTINGS`), so defining it to 0 drops the app from the
registry entirely: the "core uninstallable" pattern, with the banner's app count
following.

**App = a widget tree (#145).** Each app is a fixed status bar over per-app flex
content over a fixed tab bar, laid out by `ra8_widget_layout_stack` and drawn
through `ra8_gfx` into the GLCDC buffer. Status bar and tab bar are shared
chrome that read the registry; only the content widget differs per app.

**Input routing (#145 + #146).** SW1 and SW2 are delivered as `button` events
through `ra8_app_route_input` to the focused app, whose `on_input` launches the
neighbouring app. A touch on the tab bar goes through `ra8_widget_dispatch`,
which maps the hit column to an app id. Either way the focus lifecycle fires and
the panel re-composites visibly.

The RGB565 framebuffer lives in SRAM and composites top-left over the GLCDC
background plane; the full 1024x600 panel needs SDRAM, which is a separate task.

Before the interactive loop a deterministic self-check exercises the whole
surface and emits one banner, so the app doubles as an emulator regression gate:
all apps registered, two composites distinct, focus lifecycle fired once each, a
synthetic touch on the Library tab while Reader is focused routed all the way
through to a Library launch, and a status-bar-only invalidation yielding exactly
the status-bar rect with the `fast` hint. Anything else prints `FAIL` and parks.
It shares the GLCDC layer-1 bring-up that `glcdc_render` validates on bench.

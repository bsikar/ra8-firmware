# widget_compose_demo

A nested `ra8_widget` tree composited on the live GLCDC panel. Where
`widget_app_demo` shows the full `ra8_widget` + `ra8_app` launcher, this app
isolates the structural piece issue #145 added: **a container that is itself a
widget**, which is what turns a flat widget array into a tree.

```
root (column panel)
|- status   (leaf, fixed)   title + a live frame counter
|- body     (ROW PANEL)     a nested panel ...
|   |- left  tile (leaf, flex)
|   |- right tile (leaf, flex)
|- footer   (leaf, fixed)   a hint line
```

`body` is the primitive in action: a `ra8_widget_panel` nested inside the root
`ra8_widget_panel`. One `ra8_widget_panel_compose()` lays the children out,
computes the minimal damage rectangle and refresh hint, and renders only the
dirty children; a dirty nested panel repaints its whole subtree.

The deterministic self-check pins both ends. A full compose dirties all three
root children, flushes the whole frame with the `quality` hint, and composites
both tiles -- per-tile render counters confirm it. Bumping the frame counter and
invalidating **only** the status bar dirties exactly one child, damages just the
status rect with the `fast` hint, leaves the tiles un-rendered, and changes the
composite CRC. That is the acceptance bullet: a status-only change flushes only
the status rect. After the banner the loop keeps bumping the counter and
partial-composing, so the panel visibly updates its top band.

The RGB565 framebuffer lives in SRAM and composites top-left over the GLCDC
background plane; the full 1024x600 panel needs SDRAM, a separate task. No
SDRAM, touch, SD or buttons -- it is a pure compositor demo.

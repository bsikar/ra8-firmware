# RA8D2-### Introduce `ra_display_pal` (display abstraction layer)

**Type:** Feature
**Component:** `libs/ra_display_pal` (new)
**Priority:** Medium
**Estimate:** 1-2 days for the LCD backend + scaffold, e-ink backend deferred until hardware arrives.

---

## Summary

Add a display Platform Abstraction Layer so applications draw against a
backend-agnostic API and pick LCD or (future) IT8951 e-ink with a single
config swap. The LCD backend wraps the existing `ra_glcdc` HAL; the e-ink
backend is stubbed now and implemented when the panel arrives.

## Background

Today every GLCDC-using app repeats the same 6-step bring-up
(`ra_board_lcd_panel_power_on` -> `ra_board_glcdc_init` -> 200 ms delay ->
`ra_glcdc_init` -> `set_background_color` -> `start(true)` ->
`layer1_show`) and declares its own `static uint16_t s_framebuffer[H][W]`
with hand-tuned alignment and dimensions. That works for one chip but
makes the eventual e-ink port a per-app rewrite, and the framebuffer
geometry / alignment / address must already match what GLCDC was
configured with -- it is shared knowledge with no enforcement.

The existing `ra_net_pal` and `ra_usb_pal` libraries are the model: a
function-pointer interface + per-backend implementation, app picks
backend at init time.

## Goals

- Apps draw and refresh through one API regardless of backend.
- Backend is selected by passing a different `display_cfg_t*` to
  `display_init()` (or selecting at the app's CMakeLists level via a
  link-time choice -- pick one).
- LCD backend is a thin wrapper over `ra_glcdc` -- no behaviour change
  for `lcd_draw_x` / `threadx_guix_demo` once they are ported.
- E-ink backend compiles as a stub returning `k_ra_err_not_supported`
  for every call, so the API exists end-to-end before hardware arrives.

## Non-goals

- Drawing primitives (`set_pixel`, `line`, `text`, etc.). The PAL hands
  out a framebuffer pointer and a flush call; raw apps paint themselves
  and GUIX paints through its own driver shim.
- Porting `lcd_color_cycle` -- it animates `BG_BGC` and is intentionally
  GLCDC-specific. Document it as backend-locked.
- Driver development for the IT8951. Deferred until hardware is on the
  bench.

## Acceptance criteria

- [ ] New library at `libs/ra_display_pal/` with public header
  `inc/ra_display_pal.h` containing the interface struct, capabilities
  type, and config types.
- [ ] LCD backend at `libs/ra_display_pal/src/ra_display_pal_lcd.c`
  implementing every interface function over `ra_glcdc`.
- [ ] E-ink backend stub at
  `libs/ra_display_pal/src/ra_display_pal_eink_it8951.c` returning
  `k_ra_err_not_supported` for `display_flush` /
  `display_get_framebuffer`, so the API is callable end-to-end.
- [ ] `lcd_draw_x` ported to use the PAL. Old `lcd_bringup_panel()`
  collapses to `display_init(&k_display_cfg_lcd_ek_ra8d2_512x512)` plus
  `display_get_framebuffer()`.
- [ ] `threadx_guix_demo` ported to use the PAL. The GUIX bind step
  becomes `display_bind_guix(d, &canvas, ...)` with the GLCDC-specific
  GUIX driver hidden inside the LCD backend.
- [ ] Host unit tests under `tests/test_ra_display_pal.c` covering: init
  rejects null cfg, init returns valid caps for LCD backend, FB pointer
  is non-null and aligned, e-ink stub returns `not_supported`. MC/DC
  vectors documented per project policy.
- [ ] All CI gates pass: cross-build of the two ported apps, host
  tests, clang-tidy, doxygen, MC/DC baseline holds.

## Proposed API (draft)

```c
// libs/ra_display_pal/inc/ra_display_pal.h

typedef enum : uint8_t {
  k_display_pixfmt_rgb565 = 0,
  k_display_pixfmt_rgb888 = 1,
  k_display_pixfmt_grey4  = 2,  // packed 4bpp -- e-ink
  k_display_pixfmt_grey1  = 3,  // packed 1bpp -- e-ink fast modes
} display_pixfmt_t;

typedef enum : uint8_t {
  k_display_refresh_fast    = 0,  // e-ink: A2-style, ghosting OK; LCD: no-op
  k_display_refresh_quality = 1,  // e-ink: GC16, no ghosting;     LCD: no-op
  k_display_refresh_init    = 2,  // e-ink: full INIT clear;       LCD: no-op
} display_refresh_hint_t;

typedef struct {
  uint16_t            width_px;
  uint16_t            height_px;
  display_pixfmt_t    pixfmt;          // app-side canonical format
  uint32_t            stride_bytes;    // row stride
  uint32_t            refresh_latency_us_typ;  // 0 for LCD; ~450000 for e-ink GC16
  bool                supports_partial_update;
} display_caps_t;

typedef struct {
  void*               pixels;
  uint16_t            width_px;
  uint16_t            height_px;
  uint32_t            stride_bytes;
  display_pixfmt_t    pixfmt;
} display_fb_t;

typedef struct {
  uint16_t x, y, w, h;
} display_rect_t;

// Opaque to callers. Each backend defines the concrete type.
typedef struct display_handle display_handle_t;

// Public API the app calls.
ra_err_t        display_init(const struct display_cfg* cfg, display_handle_t** out);
ra_err_t        display_get_caps(display_handle_t* d, display_caps_t* out);
ra_err_t        display_get_framebuffer(display_handle_t* d, display_fb_t* out);
ra_err_t        display_flush(display_handle_t* d, display_rect_t r,
                              display_refresh_hint_t hint);
ra_err_t        display_clear(display_handle_t* d, uint32_t color);
ra_err_t        display_deinit(display_handle_t* d);

// Backend-specific configs live in their backend headers.
// extern const struct display_cfg k_display_cfg_lcd_ek_ra8d2_512x512;
// extern const struct display_cfg k_display_cfg_eink_it8951_<size>;
```

## Implementation steps (suggested order)

1. **Scaffold the library.** Add `libs/ra_display_pal/{inc,src}` plus
   the CMake target. No backends yet -- just the public header
   compiles.
2. **Define the interface struct.** Inside `src/`, an internal
   `display_backend_iface_t` of function pointers and the
   `display_handle` struct that holds `iface*` + backend ctx. This is
   the DI seam -- annotate with `RA_DI_SLOT("display")`.
3. **LCD backend.** Move the 6-step bring-up out of
   `lcd_draw_x/main.c::lcd_bringup_panel()` into
   `ra_display_pal_lcd.c::backend_init()`. Backend owns the static
   framebuffer (one per backend instance is fine -- no two displays on
   this chip). Returns its handle.
4. **GUIX bind helper.** Add `display_bind_guix(d, GX_CANVAS*, ...)` to
   the public header. LCD backend implements it by calling the
   existing `ra_guix_display_driver_bind`.
5. **Port `lcd_draw_x`.** Replace `lcd_bringup_panel()` and the static
   `s_framebuffer[]` declaration with PAL calls. Confirm it still
   builds, flash to board, X is still drawn.
6. **Port `threadx_guix_demo`.** Same pattern; the GUIX bind becomes
   the helper from step 4.
7. **E-ink backend stub.** New TU returning `not_supported` for
   everything but `init`/`deinit`/`get_caps`. Lets a future commit
   target it without touching apps.
8. **Tests.** New `tests/test_ra_display_pal.c` driving the LCD backend
   under the simulator (`ra_sim_mmap`) and the e-ink stub for the
   not-supported path. Match the per-test MC/DC block style.
9. **Docs.** Add a short section in `docs/ARCHITECTURE.md` describing
   the PAL, plus a `docs/DRIVER_STATUS.md` row for the e-ink backend
   (status: `STUB`).

## Open questions to settle before coding

- **Backend selection: runtime or link-time?** Runtime (pass cfg
  pointer) is more honest about it being DI; link-time (pick at app's
  `CMakeLists.txt`) is one-line cleaner and saves flash. Pick one and
  document.
- **Multiple FB sizes per backend?** `lcd_draw_x` uses 512x512,
  `threadx_guix_demo` uses 480x270. Either each app passes dimensions
  in `display_cfg_t`, or there is one canonical size per backend
  config. First is more flexible; second is simpler. Probably do
  first -- otherwise we end up with `k_display_cfg_lcd_512x512`,
  `k_display_cfg_lcd_480x270`, `k_display_cfg_lcd_1024x600`...
- **What does `display_clear()` do on e-ink?** Plausibly: write
  background to FB then `flush(full, k_display_refresh_init)`. Or omit
  `display_clear` entirely and make apps do it themselves.

## Risk / caveats

- The existing GLCDC bring-up has subtle ordering requirements (panel
  power -> GLCDC pin/clock setup -> 200 ms settle -> init -> bgc ->
  start -> layer1_show). The LCD backend MUST keep this order; do not
  refactor while moving.
- `lcd_color_cycle` will not be ported. Mark it `BACKEND_LOCKED: lcd`
  in its README so future readers know not to "fix" it.
- The IT8951 typically attaches over SPI; SPI bring-up + framebuffer
  conversion (RGB565 -> 4bpp luma) belong in the e-ink backend's
  `flush()`, not anywhere the LCD backend can see. Keep the boundary
  clean.

## References

- Existing PAL examples: `libs/ra_net_pal/`, `libs/ra_usb_pal/`
- DI annotation: `RA_DI_SLOT(role)` in `libs/ra_core/inc/ra_attributes.h`
- Apps to port: `examples/ek_ra8d2/hw_validated/manual/lcd_draw_x/`,
  `examples/ek_ra8d2/hw_validated/manual/threadx_guix_demo/`
- GUIX driver shim: `port/guix/` and
  `libs/ra_hal/inc/gx_display_driver_ra_glcdc.h`
- IT8951 datasheet: not yet committed under `docs/reference/` -- add
  when hardware order arrives.

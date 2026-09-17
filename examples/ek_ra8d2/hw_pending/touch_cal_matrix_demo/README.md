# touch_cal_matrix_demo

First consumer of `libs/ra8_touch_cal`.

The library is pure math behind two caller-supplied shims (a cross-hair
painter and a raw-sample reader), so nothing about it needs a panel or a
touch controller. This app supplies a deterministic fake panel and walks all
four public entry points:

1. `ra8_touch_cal_run` paints the five built-in targets (four corners inset by
   40 px plus the centre). The draw shim records the cross-hair it was asked
   to paint; the read shim answers with the raw sample a controller obeying a
   known affine map would report for that cross-hair. Both shims count their
   calls, so the leg also proves the library painted before it sampled, five
   times.
2. The solved matrix is compared against the ground truth the fake controller
   was built from: gain 1/5 on both axes, bias -20 px in X and -16 px in Y.
3. `ra8_touch_cal_apply` maps a fresh raw sample (2100, 1216) and the result
   is checked against the hand-computed pixel (400, 227).
4. `ra8_touch_cal_save` serialises the matrix, the `TCAL` magic and version
   byte are checked in place, and `ra8_touch_cal_load` reads it back for a
   coefficient-by-coefficient compare.

## What it does not touch

No panel driver, no GT911, no storage. The 36-byte blob lives on the stack of
the call that makes it. That width is exactly the touch-calibration window of
a `ra8_devcfg` record, which is where a real product would park it; wiring
those two together is a separate change.

## Build and run

```
just apps::example::build touch_cal_matrix_demo
```

Console is SCI8 on the J-Link OB VCOM. A good run prints:

```
touch_cal_matrix_demo: boot
touch_cal_matrix_demo: run+solve PASS
touch_cal_matrix_demo: apply PASS
touch_cal_matrix_demo: blob PASS
touch_cal_matrix_demo: ALL PASS
```

## Status

`hw_pending`: compiles against the pinned arm-gnu-toolchain 13.3.rel1 under
the `ra8d2-debug` preset (12428 B text, 1296 B bss), not yet run on a board.
Every leg is deterministic and self-checking, so the run needs a board only to
confirm the console path.

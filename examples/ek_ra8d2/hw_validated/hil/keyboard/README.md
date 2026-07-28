# keyboard

On-silicon **HIL gate** for the on-screen keyboard widget `ra8_keyboard` (#105) --
the text-entry model behind the e-reader Library search.

## What it does

`ra8_keyboard` is a pure, rendering-free model modelled on the iOS keyboard: a
**letters** layer (`qwertyuiop` / `asdfghjkl` inset / SHIFT + `zxcvbnm` +
BACKSPACE / 123 + SPACE + RETURN) and a **numbers** layer (digits + symbols),
toggled by the 123 / ABC key. It lays the active layer into a frame as
`ra8_ui_rect_t` key rects, maps a tap to a key (`ra8_kbd_hit`, via the shared
`ra8_ui_rect_contains`), and mutates a text buffer + one-shot SHIFT + active
layer (`ra8_kbd_apply`). The caller owns drawing + tap routing; this is the
deterministic logic underneath.

This HIL drives that model with **synthetic key-centre taps** -- the same
input-injection pattern as `ereader_input` (#118), for text entry. It types
`Hi 9` -- exercising one-shot SHIFT (capital `H`), SPACE, and the `123` layer
toggle to reach a digit (`9`) -- commits with RETURN, asserts the result, and
prints on the SCI8 J-Link OB console:

```
kbd: q=Hi 9 commit=1 taps=7 PASS
```

No panel / SD / touch hardware is needed (pure layout + hit-test + buffer), so
the banner is identical on host, `ra8_emulator`, and silicon. A mismatch prints a
FAIL banner and halts on a BKPT before the PASS line.

## Build + run

```
make keyboard
scripts/hil/run_local.sh keyboard      # flash + scrape the banner
```

## Result (validated 2026-06-19, ra8_emulator)

```
keyboard-hil: boot
kbd: q=Hi 9 commit=1 taps=7 PASS
```

`scripts/emu/smoke.sh keyboard` PASS (final PC in the `main` WFI idle
loop; 7 synthetic taps routed through `ra8_kbd_hit` -> `ra8_kbd_apply`). The
widget logic -- case toggle, the 123/ABC layer switch, digits, edits -- is
covered on the host by `tests/test_ra8_keyboard.c` (ASan + MC/DC for the
frame-rejection decision); the same model + the same sequence produce the same
banner, so host == ra8_emulator == silicon.

## Relationship to #105

This gate validates the keyboard **widget** (layout / hit-test / text buffer) on
silicon. The remaining `#105` acceptance item -- wiring `Search` in the
`ereader_ui` Library toolbar to open this keyboard and filter the shelf, plus a
golden snapshot of the rendered keyboard screen -- consumes this same model.

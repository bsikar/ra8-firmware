# keyboard

On-silicon gate for the on-screen keyboard model `ra8_keyboard` (#105), the
text-entry logic behind the e-reader Library search.

The widget is pure and rendering-free: a letters layer and a numbers layer
toggled by the 123 / ABC key, laid into a frame as key rects, with a tap mapped
to a key and applied to a text buffer plus a one-shot SHIFT and the active
layer. The caller owns drawing and tap routing.

This app drives that model with synthetic key-centre taps -- the same
input-injection pattern `ereader_input` (#118) uses -- typing a short string
chosen to force one-shot SHIFT, SPACE and the layer toggle to reach a digit,
then committing with RETURN and asserting the result. A mismatch halts on a
BKPT before the PASS line can print.

No panel, SD or touch hardware is in the path, so the banner is identical on
host, emulator and silicon. That is what makes it a gate rather than a
rendering test. The same model is covered on the host by
`tests/security/src/test_ra8_keyboard.c`.

# examples/ek_ra8d2/hw_pending/

Apps that compile and pass every CI gate but have not been confirmed working on
hardware end to end. Each app's own README carries its blocking reason and the
evidence gathered so far.

Most of them do boot headlessly in `tools/ra8_emulator`. What is missing is
usually the thing an emulator cannot model -- an analog comparator, an ECC bit
flip, a rasterizer, a scope on a PWM edge, a reset-retained power domain -- which
is why they are pending rather than validated.

`just apps::build <appname>` builds any of them from the repo root. Promotion is a `git mv`
into [`../hw_validated/hil/`](../hw_validated/hil/) with a `hil.conf` beside
`examples/ek_ra8d2/hw_validated/hil/<app>/src/main.c` once a hardware probe
passes; discovery is the filesystem, so nothing
else needs editing.

Two subdirectories group by lane rather than by subject: `c6/` needs the ESP32-C6
companion radio, and `manual/` needs a human at the board or a PC on the far end
of a cable. `common/` is shared app code, not an app.

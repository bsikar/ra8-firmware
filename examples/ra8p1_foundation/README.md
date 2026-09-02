# examples/ra8p1_foundation/

The second target. These apps prove the platform is genuinely multi-chip rather
than an RA8D2 codebase with a device macro, and they exercise the Ethos-U55 NPU
-- the RA8P1's defining feature over the RA8D2.

They climb in order: first that `ra8_core` and `ra8_hal` compile and link for
the RA8P1 at all, then the bare NPU command/queue driver, then loading a
committed model container through the on-target loader instead of hand-building
a command stream, and finally the pieces a TFLite-Micro Ethos-U runtime needs
above the driver.

There is no RA8P1 board on the bench, so the whole tier is gated by building,
by the host tests, and in the emulator. `just apps::build <appname>` from the repo root,
same as any other example.

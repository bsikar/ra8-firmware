# c6_hosted_init

First consumer of `port/esp-hosted/`, the first-party RA8D2 + ThreadX port of
the vendored esp-hosted host driver, and its bring-up harness. It initialises
the port, prints the pin map and interrupt routing the port itself resolved,
reads both side-band lines through the OS-abstraction vtable, clocks exactly one
full-duplex transaction and decodes the frame that comes back.

No EK-RA8D2 pin number appears in this app's sources: it prints whatever the
port's pins header resolves to. That header and the co-processor's `pins.env`
are the only two places the map lives and a CI checker diffs them, so a third
copy in prose is exactly the drift this app is built to avoid.

## Side-band routing: one hardware edge, one polled pin

The ICU external-interrupt inputs are concentrated on port 0 in this package, so
of the four Pmod1 side-band nets only the one carrying HANDSHAKE has an IRQ
channel. That is the right way round, and not by luck:

- HANDSHAKE needs a real edge. The co-processor image deasserts it for the
  length of a chip-select assertion, so a poll can miss the pulse outright.
- DATA_READY can be polled. The C6 holds it asserted until the host drains the
  queued frame, so a software edge detector on a bounded timer can be late but
  cannot miss the condition. It raises the same callback, and the vendored
  driver never sees the difference.

## Where ThreadX object creation has to happen

`ra8_esp_hosted_port_init()` creates ThreadX byte pools, mutexes and timers, so
it cannot run before the kernel. `main()` does only the bare-metal half --
clocks, module-stop, SysTick, console -- and then enters the kernel; port init
runs from `tx_application_define()`, which registers the event handler first.
The worker thread is the earliest context that can print, so it reports the
exact `ra8_err_t` the init returned.

## An idle filler frame is a PASS, not a malformed one

A verdict needs the transfer to have succeeded, the receive buffer to have
actually been driven, and the frame to be either a fully verified data frame or
the co-processor's idle filler. The filler legitimately carries a zero payload
offset, because it has no payload to point at; the first silicon run judged one
by the rules for a data frame and reported a header failure at a link that was
working perfectly.

A freshly-booted C6 reads DATA_READY asserted because it is still holding its
queued boot event, and the first completed transaction drains that for good.

## Reading a FAIL

An all-zero receive buffer is what a *connected* co-processor that never saw a
valid transaction looks like: it holds its controller-in line with an internal
pull-down, so either chip select never asserted for the frame or it was clocked
in a mode the C6 rejects. An all-ones buffer means the RA8 input floated -- the
switch bank, an unpowered C6, or a lifted joint. Either way `c6_spi_probe` is
what separates the wire from the firmware, so run that next.

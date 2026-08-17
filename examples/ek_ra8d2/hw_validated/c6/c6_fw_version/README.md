# c6_fw_version

Proves the esp-hosted *protocol*, where `c6_spi_probe` and `c6_hosted_init` stop
at "bytes moved and their shape is sane". It sends a request the co-processor
must parse, decodes the response with the vendored protobuf codec, and then
checks what is in it: the UID echoed back (which is what makes it a round trip
rather than a coincidence), the result code, the firmware version and the chip
id.

The request is not hand-encoded. Hand-encoding would only prove that this app
and the co-processor agree; the generated codec agreeing with the co-processor
is a far stronger statement.

## The version lock

The version expectation is not a number written down here -- it is the vendored
host driver's own version, read from its header. Host and co-processor come from
one pinned upstream commit and must agree exactly, so reflashing the C6 without
bumping the vendor pin, or bumping the pin without reflashing, turns this app
red on purpose instead of surfacing later as unexplained RPC timeouts.

The co-processor's IDF target string is printed but never asserted, through a
formatter that truncates and strips non-printable bytes: it is a string the far
side chose, and a protocol field that can reprogram a terminal is not evidence.

## The ESP_PRIV_IF INIT event is unusable on this co-processor build (#529)

The C6's bootup INIT frame -- the one carrying its capabilities, chip id and
firmware version as TLVs -- is checksummed with `if_num` taken as zero and then
transmitted with a non-zero `if_num`. The shortfall is exactly `if_num << 4`.
Every other frame it sends carries `if_num = 0` and verifies, so this is not a
receive-path fault on the RA8 side. Any conformant host drops the frame,
including upstream's own `process_spi_rx_buf()`.

This app diagnoses the condition by name and counts it, but does not accept the
frame: decoding bytes that fail the integrity check shipped with them would mean
trusting an unverified header. Nothing above this layer may therefore be built
on the INIT event's TLVs. Nothing is lost either -- the boot announcement also
arrives as an RPC event that does verify, and the version request returns the
same three facts on demand.

The TLV decoder for that event is consequently correct and unexercised on this
bench. It is kept rather than deleted because it is a real implementation of a
real protocol path that runs the moment the co-processor firmware is fixed.

## Why parts of the vendored driver are restated rather than compiled

The protobuf message and its codec are vendored and compiled. The TLV envelope
and the payload header / checksum / transaction are first-party mirrors of
vendored code that cannot be built here: the vendored send path expands an
allocation whose failure arm is a `goto` (NASA Rule 1), and the vendored
transaction task is written against ESP-IDF's Wi-Fi API. Restating six bytes of
framing beats importing a rule violation. SPI, GPIO and ThreadX objects are
reached only through the vendored `g_h.funcs` vtable, so the pump sees exactly
what the vendored driver would see.

Unpacking a protobuf needs an allocator and this image has no heap -- `_sbrk` is
a strong symbol that reports a fatal error. The decoder is handed an allocator
backed by the esp-hosted port's fixed ThreadX byte pool, carved once at init,
which is what keeps the decode inside NASA Power of 10 Rule 3. Every unpacked
message is freed on every path.

`docs/SOUP/esp-hosted-host.md` records which vendored translation units are
compiled and why the rest are not.

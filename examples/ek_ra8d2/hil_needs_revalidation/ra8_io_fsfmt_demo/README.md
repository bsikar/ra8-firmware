# ra8_io_fsfmt_demo

Drives the `ra8_io` filesystem-format registry (#159) with no external hardware.

The registry lets the fabric recognise the on-disk filesystem on a block device
and report what that format can do, so the upper layers never hard-code a
`switch` over FAT versus exFAT. Each format supplies a `probe` -- does this
volume look like me? -- and a capability descriptor: read-only or writable,
sub-directories, streaming writes, maximum name length.

Two halves:

- **The built-in probe.** A FAT12 volume is formatted on a RAM block device,
  then `ra8_io_fsfmt_probe` resolves it to the `"fat"` format and its advertised
  capabilities are checked against what FAT can actually do.
- **The foreign-format seam.** A stub format whose `probe` claims any volume
  carrying a magic byte in block 0 is registered through
  `ra8_io_fsfmt_register`; a second RAM device marked with that byte resolves to
  the stub, with the stub's own read-only and case-sensitive capabilities. That
  is the load-bearing claim: a new format plugs in with **no change** to the
  built-ins.

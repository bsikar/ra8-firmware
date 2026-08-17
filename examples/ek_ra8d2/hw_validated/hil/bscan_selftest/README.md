# bscan_selftest

Self-test for the JTAG / IEEE-1149.1 boundary-scan bookkeeping driver
`ra8_bscan` (#138). No panel, SD card, touch or external JTAG fixture needed.

There is no such thing as a "real" boundary-scan demo on this chip. The TAP is
driven by an external manufacturing fixture over TCK/TMS/TDI/TDO while RES is
held low, and its four registers (JTIR / JTIDR / JTBPR / JTBSR) are not
memory-mapped -- HUM Ch 50.2.3 p 3259 is explicit that the CPU can neither read
nor write them. Exercising the actual scan chain is therefore inherently
external-tool work: a BSDL file plus a JTAG controller. What firmware can
validate is its own contribution, the `ra8_bscan` bookkeeping object.

The app runs that contract on both its positive and negative paths: the
hardwired device ID code `0x085DA447` (HUM Ch 50.2.2 p 3258), which is the value
the external fixture should scan out over TDO and so is the firmware's
authoritative cross-check against it; acceptance of the named JTIR opcodes and
rejection of the reserved 4-bit ones (HUM Ch 50.2.1 p 3258); and the status and
clear-mask guards. It halts on a FAIL banner *before* the PASS line can be
reached, so the gate is exact.

Because the driver touches no hardware at all, the banner is byte-identical on
host, emulator and silicon, which makes this an emulator/silicon equivalence
check as much as a driver test. The same logic is covered from the other side by
the host unit tests in `tests/test_ra8_bscan.c`.

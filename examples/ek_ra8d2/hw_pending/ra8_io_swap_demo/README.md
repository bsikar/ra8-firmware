# ra8_io_swap_demo

The `ra8_io` capstone (epic #155, #264): a single binary showing every
abstraction at once -- two block-device backends swapped behind one interface, a
VFS round-trip, and stdio retargeted to two different sinks. Where each sibling
`ra8_io_*_demo` binds one backend, this ties the fabric together.

1. **Drop-in block-device swap.** One backend-agnostic engine runs the
   *identical* round-trip over a table of backends. Row 0 is an in-SRAM RAM block
   device: volatile, erases to zero, no erase-before-write. Row 1 is the on-board
   Octo-SPI NOR flash: an erase-before-write medium that reads back `0xFF` after
   erase and does a whole-sector read-modify-write on every 512-byte block write.
   The engine names no peripheral, so two capability-different media are proven
   interchangeable behind the one vtable; adding a third is one more table row and
   the engine body never changes.
2. **VFS open / read / write / close.** Each backend is bridged to `ra8_fs`,
   formatted and mounted as FAT12, and registered in the VFS under a short name.
   The round-trip opens for write, writes, closes, re-opens for read, reads,
   closes and byte-compares -- the streaming file API, not a whole-file shortcut.
3. **Targetable stdio, retargeted to two sinks.** During the swap phase the
   engine's progress stream is an in-RAM capture sink, so the same `puts` /
   `put_u32` calls that would otherwise reach the serial console land in a byte
   buffer; afterwards the captured bytes are replayed out of the UART sink. One
   writer, two destinations.

The engine logic, the deterministic payload pattern and the read-back verdict are
covered on the host in `tests/test_app_ra8_io_swap_demo.c`, which runs the same
swap over a RAM backend and the register-level xSPI NOR model plus the in-RAM
stdio sink, with MC/DC vectors for the compound read-back verdict and the
pattern-equality guard.

## Blocked on

The xSPI leg. It programs the *non-volatile* on-board OSPI NOR and has not been
captured on the bench; it is emulator-proven and register-identical to
`ra8_io_xspi_demo`, but only a bench run against the real IS25LX512M closes it.
The RAM, VFS and stdio legs are fully exercised off-target and on the host.

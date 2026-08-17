# ra8_io_demo

The reference end-to-end run through the whole `ra8_io` I/O fabric (#155), with
no external hardware:

1. a RAM block device over an in-SRAM buffer (#156);
2. that block device bridged to `ra8_fs` (#158), formatted and mounted as FAT12
   and registered in the VFS under a name;
3. a file written and then read back through its `"ram:/..."` VFS path and
   byte-compared;
4. progress printed over the SCI8 console through a `ra8_io_stream` UART sink,
   with `ra8_log` routed into the same stream via `ra8_io_log_attach`.

Because the backend is pure memory, this is the app that isolates fabric
defects from media defects: everything it exercises is the fabric itself. Its
siblings swap only step 1 -- `ra8_io_sd_demo` for a card, `ra8_io_sdram_demo`
for external SDRAM, `ra8_io_xspi_demo` for OSPI NOR -- which is the whole point
of the swappable-backend design.

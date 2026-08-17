# ra8_io_sd_demo

Proves the `ra8_io` fabric's swappable-backend promise (#155, #156): the same
VFS API that `ra8_io_demo` runs over a RAM disk, running over a microSD card by
swapping only the block device. Everything above `ra8_io_blockdev_sdspi_init` is
identical -- the `ra8_fs` bridge, the format and mount, the VFS mount, `mkdir`, a
write, a read-back and a byte-compare against a deterministic payload. Every
return value is checked and the first failing step parks the CPU.

The SD bring-up itself -- the Pmod2 / SCI0 Simple-SPI transport adapter and card
init -- is reused verbatim from `fs_format_mount` rather than reimplemented.

> **This app erases the card.** It reformats the volume. Use a disposable
> microSD.

An unseated or missing card is the first thing to rule out when it fails.

## Hardware

| EK-RA8D2 pin | PMOD MicroSD signal | Net (EK-RA8D2 v1 UM Table 19 p 27) |
|--------------|---------------------|------------------------------------|
| Pmod2.1 P604 | CS                  | SSLB0 (GPIO output)                |
| Pmod2.2 P603 | COPI                | MOSIB (UM legacy name)             |
| Pmod2.3 P602 | CIPO                | MISOB (UM legacy name)             |
| Pmod2.4 P601 | SCK                 | RSPCKB                             |
| Pmod2.5 GND  | GND                 | GND                                |
| Pmod2.6 3V3  | 3V3                 | 3V3                                |

# fs_format_mount

Formats, mounts and exercises a real microSD card in every filesystem `ra8_fs`
can write -- FAT12, FAT16, FAT32 and exFAT -- one after another. It assumes no
pre-existing layout: for each type it runs `ra8_fs_format()`, mounts and asserts
the detected type matches, then creates, writes, reads back and byte-compares a
deterministic payload, renames the file and re-reads it intact, unlinks it and
unmounts. This is the on-hardware exercise for the `ra8_fs_format()` mkfs API.

> **This app erases the card.** Insert a disposable one.

The exFAT trial additionally asserts an empty root directory twice: immediately
after the format, where the bitmap, up-case and volume-label system entries must
stay hidden, and again after the unlink, which proves the directory-set teardown
fully reclaimed the entry.

Any card of roughly half a megabyte or more works. The formatter's auto
cluster-size sweep lands each type's cluster count in its valid band -- FAT12
below 4085 clusters, FAT16 4085 to 65524, FAT32 65525 and up -- but on a typical
multi-gigabyte card FAT12 and FAT16 exceed their ceilings even at the maximum
cluster size. A type that cannot fit the card is **skipped, not failed**, so the
run still ends in an all-pass, which means a large card silently exercises fewer
paths than a small one. Use a small card, or a small emulator-attached card, to
cover every type.

## Hardware

A Digilent PMOD MicroSD (part 410-380) in Pmod2 (J25), driven by SCI0 in
Simple-SPI mode.

| EK-RA8D2 pin | PMOD MicroSD signal | Net (UM Table 19 p 27) |
|--------------|---------------------|------------------------|
| Pmod2.1 P604 | CS                  | SSLB0 (GPIO output)    |
| Pmod2.2 P603 | COPI                | MOSIB (UM legacy name) |
| Pmod2.3 P602 | CIPO                | MISOB (UM legacy name) |
| Pmod2.4 P601 | SCK                 | RSPCKB                 |
| Pmod2.5 GND  | GND                 | GND                    |
| Pmod2.6 3V3  | 3V3                 | 3V3                    |

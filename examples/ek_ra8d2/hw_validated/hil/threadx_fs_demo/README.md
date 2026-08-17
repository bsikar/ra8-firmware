# threadx_fs_demo

FAT file operations under ThreadX against the on-board Octo-SPI NOR flash, via
LevelX wear-levelling. No SD card is involved.

It was an SD-card demo once, but the board's microSD hangs off Pmod2 / SCI0
Simple-SPI and needs a module plugged in, whereas the OSPI flash is soldered
down and always present -- so this runs unattended on the bench with no card
and no jumpers. The worker thread lays down a LevelX partition, formats and
mounts a FAT volume on top of it, then creates two files, lists the root, reads
one back and **verifies the bytes match**, deletes the other and confirms it is
gone.

The read-back compare is what makes it a test rather than a smoke check: a
backend that writes into a void and returns success passes everything up to
that point.

This is the file-operations counterpart to `threadx_fs_levelx_demo`, which
proves the LevelX integration itself. It also exercises the `ra8_fs_set_lock()`
seam bound to a ThreadX mutex (#608). Both demos ran on the vendored FileX
until #611 retired it -- `ra8_fs` covers the whole surface they used.

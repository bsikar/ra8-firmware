# threadx_levelx_demo

LevelX wear-levelling burn-in on the on-board Octo-SPI flash, under ThreadX.
The worker formats and opens a LevelX partition, then heartbeats: program an
incrementing counter into logical sector 0, read it back, compare. After many
thousands of cycles it dumps LevelX's statistics -- write and read counts,
min and max erase counts, free / mapped / obsolete physical sector counts,
cache hit and miss counts.

The burn-in is the point. A wear-levelling layer that survives one write is
indistinguishable from one that does not level at all; what has to be shown is
that the writes were spread across every block rather than grinding one past
its erase budget, and the erase-count spread in that dump is where you see it.

`threadx_fs_levelx_demo` puts a FAT volume on top of the same stack; this app
stays at the raw LevelX sector layer.

## Hardware notes

The board carries an ISSI IS25LX512M-JHLE Octo-SPI flash (EK-RA8D2 v1 UM
Section 6.3 and Table 29 p 35; JEDEC ID 0x9D5A1A, hardware-verified). It hangs
off xSPI controller **CS1**, not CS0 -- see #44 for the bring-up fix. SW4-3
selects Octo-SPI over the Arduino / Pmod1 routing, so those pins are not
simultaneously available.

The LevelX NOR driver owns the xSPI bring-up. The app must not also route the
pins or init the controller, or the duplicate PFS route is rejected and the
format fails a long way from its cause.

This app cannot link under the default bare-metal configuration: it needs
ThreadX and LevelX configured in, which its own build forces on.

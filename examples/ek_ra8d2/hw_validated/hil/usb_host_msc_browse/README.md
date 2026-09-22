# usb_host_msc_browse

Validates the first-party USB **host** MSC stack (`ra8_usb_hmsc`) with no
external drive: the board hosts on one jack and emulates the peripheral on the
other over the loop cable (J7 HS host <-> J11 FS device), one image running both
roles.

The device side exposes the 1 MiB MRAM window at `0x02000000` as a read-only
synthesized FAT16 volume. The host side mounts it and *browses* -- reads the
root directory over `READ(10)` and parses the file entry, name and size --
before the raw byte-for-byte read-back and the write-protect check.

The directory browse is what separates this from the raw read-verify self-loops;
`usb_selftest_hs_host` covers those, and the pinout and VID/PID match it. The
original version needed a real USB thumb drive in J7; the self-loop stands in
for it, so this runs unattended.

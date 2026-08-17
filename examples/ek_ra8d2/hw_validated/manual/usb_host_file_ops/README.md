# usb_host_file_ops

Native USB **host-mode** file-operations exerciser. Plug a thumb drive into
either jack -- the firmware alternates between the USB-HS port (J7) and the
USB-FS port every retry cycle until a drive answers -- and it mounts the volume
through `ra8_fs`, with block I/O bridged onto the `ra8_usb_hmsc` SCSI
READ(10)/WRITE(10) calls. It then runs every core file operation against the
real medium with a printed verdict per step: write a file, read it back and
compare byte for byte, list it, rename it, prove the old name is gone and the
new one intact, unlink it, prove it is gone. LED2 toggles per step and LED1
lights solid on a full pass.

The suite is self-cleaning -- the drive's directory is bit-identical before and
after. Existing data is never touched, but it does write to a real removable
medium, so do not use a drive whose contents you cannot afford to risk.

What that exercises, on real hardware rather than a fixture, is `ra8_fs`
partition discovery down to a GPT layout (protective MBR type 0xEE -> `EFI PART`
header -> Basic Data entry preferred over the EFI System Partition), the exFAT
write path (directory entry-set creation plus allocation bitmap), listdir, the
in-place exFAT rename (Stream NameLength / NameHash patch, Name entry rebuild,
SetChecksum) and unlink (in-use bit clear, cluster free). If the mount fails,
the firmware dumps the partition table and the first few sectors, so an
unsupported layout is identifiable straight from the console log.

It is manual because the bench has no USB gadget that can present itself as a
mass-storage device, and because the suite deliberately mutates a real one. CI
can build it; a human has to insert a drive and read the final verdict.
`usb_host_msc_browse` is the layer below -- the raw enumerate / INQUIRY / READ
ladder.

## Board facts for host mode

- SW4-8 must be in the Host position, which the app sets through the U15 I/O
  expander rather than asking for a switch flip.
- J7 host power is a GPIO driven high, gating U18's 2 A budget (EK-RA8D2 v1 UM
  Sec 6.2 p 34).
- The USBHS VBUS sense pin is the only PFS-muxed USBHS pin (UM Table 28 p 34);
  D+ and D- are dedicated PHY balls.
- LEDs are per UM Table 24 p 31, and the log goes out on SCI8 to the J-Link OB
  CDC port.

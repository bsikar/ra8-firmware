# usb_msc_mram

Plug the board's USB-FS port into any computer and the chip's on-board MRAM
shows up as a file. The board enumerates as a mass-storage device whose single
LUN is a synthesized read-only FAT16 volume: the boot sector, FAT and root
directory are generated on the fly inside the media-read callback, and the data
clusters map 1:1 onto the MRAM code window at `0x02000000`. The root holds one
file, `MRAM.BIN` -- open or copy it and you are reading the chip's own flash,
live.

The LUN reports write-protected via MODE SENSE and rejects WRITE(10) with DATA
PROTECT sense, so hosts mount it read-only and the MRAM is never touched. It is
built on the same ThreadX + USBX storage scaffold as `usb_msc_device`;
`usb_msc_mram_hs` is the high-speed twin.

The synthesized data region is padded well past the end of the file so that the
cluster count crosses the FAT16 threshold of 4085 clusters (MS FAT spec 1.03 sec
3.5) -- below it the volume is FAT12 and hosts reject the geometry.

It is manual, and the check worth doing by hand is the honest one: copy
`MRAM.BIN` off over USB, dump the same window over SWD with the debugger's
`savebin`, and compare. Identical bytes mean the whole path -- FAT synthesis,
SCSI, the device-controller bridge, the host -- is telling the truth. LED1
toggles per SCSI read (EK-RA8D2 v1 UM Table 24 p 31).

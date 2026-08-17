# usb_audio_device

Brings the USB Full-Speed controller up in device mode through the hand-written
`ra8_usb` stack and enumerates the board as a USB Audio Class 1.0 device,
feeding a precomputed 1 kHz sine into the isochronous IN endpoint every frame
so a host can render the tone. It uses the on-board USB-FS receptacle, not the
High-Speed one.

The stream is Type-I PCM at 48 kHz, 16-bit, stereo, which puts one full frame's
worth of samples in each isochronous packet. The sine table is at half
full-scale so a naive host-side mixer downstream cannot clip it.

The USB-FS pin set is not a choice: it is the only routing the chip exposes for
the on-board Type-C Full-Speed receptacle, so there is nothing to configure
(EK-RA8D2 v1 UM Table 22 "USB Full Speed Port Pin Assignments" p 30).

Descriptor and format handling follow USB Audio 1.0 section 2.2.5 "Format Type
Descriptor"; the controller is the HUM's USBFS chapter. If the device
enumerates under an unexpected VID/PID, the audio descriptor table in the USB
driver is what still needs wiring -- this app documents the design intent
ahead of that.

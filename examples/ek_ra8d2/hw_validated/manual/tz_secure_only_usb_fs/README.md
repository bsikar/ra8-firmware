# tz_secure_only_usb_fs

USB-FS device-mode CDC ACM echo running entirely in the Secure world: no
TrustZone partition and no NSC veneers. The device enumerates as a virtual
serial port and echoes every byte sent on bulk-OUT back on bulk-IN.
`tz_secure_only_usb_hs` is the high-speed twin on the other connector.

Both USB cables have to be plugged into the host: J10 for the J-Link OB
(flashing and RTT), J11 for the CDC enumeration. VBUS on J11 comes from the
on-board USB-PD controller -- the host PC does not power that rail.

It is manual because a PC at the far end has to open the port and round-trip a
payload. There is a helper for that under `scripts/hil/usb/`.

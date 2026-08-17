# tz_secure_only_usb_hs

USB-HS device-mode CDC ACM echo running entirely in the Secure world: no
TrustZone partition and no NSC veneers. The device enumerates as a virtual
serial port and echoes every byte sent on bulk-OUT back on bulk-IN.
`tz_secure_only_usb_fs` is the full-speed twin on the other connector.

Both USB cables have to be plugged into the host: J10 for the J-Link OB
(flashing and RTT), J7 for the CDC enumeration. J7 is the high-speed port with
the on-board UTMI PHY, and its VBUS comes from the on-board USB-PD controller --
the host PC does not power that rail.

It is manual because a PC at the far end has to open the port and round-trip a
payload. There is a helper for that under `scripts/hil/usb/`. Plug J7 into a
port that genuinely advertises USB 2.0 high speed: some USB-3 hubs quietly
negotiate the device down to full speed, which looks like a firmware fault and
is not one.

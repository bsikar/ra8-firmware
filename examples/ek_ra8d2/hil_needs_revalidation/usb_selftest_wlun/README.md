# usb_selftest_wlun

The write-path counterpart to the read-only MSC self-loops: it exercises the
host-to-device bulk-OUT data phase (SCSI `WRITE(10)`). The two USB ports are
cabled to each other and one firmware image runs both stacks.

USBFS on J11 is the **device**: a ThreadX + USBX Mass-Storage class exposing a
single writable logical unit backed by a small RAM disk, whose `media_write`
copies host data in and whose `media_read` serves it back. USBHS on J7 is the
polled first-party **host**: it enumerates the device, `WRITE(10)`s a
deterministic per-LBA pattern across the whole disk in bursts, then `READ(10)`s
it back and byte-checks every sector against the same pattern.

Because the host writes the data and reads it back, the loop proves the device
bulk-OUT WRITE data phase round-trips intact over USB on chip -- the capability
that gates writable OSPI, CDC and HID. `s_dbg_mismatch` holds the first
differing sector, or an all-ones sentinel when every sector matched.

## The four things a bulk-OUT data phase needs

All four have to be right together; any one missing stalls or corrupts the
transfer, and this app is what proves them on hardware.

1. **Host MPS.** Chunk the data-out at the *device's* enumerated endpoint
   `wMaxPacketSize` (64 at full speed), not at the host controller's speed
   ceiling (512 at high speed). `ra8_usb_host_bulk_out` ships one packet per
   call.
2. **Device DBLB.** Double-buffer the device bulk-OUT pipe so the host's next
   packet lands in bank B while the ISR drains bank A.
3. **Loop-drain.** The DCD drains every ready OUT bank per interrupt, not just
   one.
4. **Single-chunk buffer.** `UX_SLAVE_REQUEST_DATA_MAX_LENGTH = 4096` keeps a
   typical WRITE inside one device transfer, avoiding an inter-chunk gap.

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO low, P8_14/P8_15 data (PSEL
usb_fs). HS host: SW4-8 to Host via the U15 expander, PD07 high so U18 powers
J7, P4_08 VBUS sense (PSEL usb_hs). Console on PD_02/PD_03, SCI8.

The device side advertises VID 0x1209, PID 0x0014. Bench use only.

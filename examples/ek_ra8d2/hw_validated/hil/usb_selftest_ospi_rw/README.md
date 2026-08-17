# usb_selftest_ospi_rw

The **non-volatile write-path** member of the USB self-loop matrix: it exercises
the host-to-device bulk-OUT data phase (SCSI `WRITE(10)`) landing on the onboard
OSPI flash. The two USB ports are cabled to each other and one image runs both
stacks -- USBFS (J11) exposes a single writable logical unit backed by a 32 KiB
window of the IS25LX512M at xSPI CS1, USBHS (J7) is the polled first-party host
stack, which writes a deterministic per-LBA pattern across the window in bursts
and then reads it back and byte-checks every sector. Because the host writes the
data and reads it back off real flash, this is the validation that the device
bulk-OUT WRITE data phase round-trips onto *persistent* storage rather than RAM.

## The OSPI window and why the write path is program-only

The device worker provisions OSPI once at boot: activate the octo-SPI mux via
the U15 I/O expander, init the xSPI pins, `ra8_xspi_init` in 1S-1S-1S, read the
JEDEC id, then **erase the whole window once**. Each subsequent `WRITE(10)` is
therefore a fast program-only path -- a per-write 4 KiB erase would blow the
host's BOT timeout.

The window at `0x00200000` is scratch, clear of both `flash_journal` at offset 0
and the read-only image `usb_selftest_ospi` writes at `0x00100000`. It is erased
and rewritten every run, so the test never touches another app's data.

## The driver fix this validates

A host-to-device bulk-OUT data phase needs four things working together, all in
`ra8_usb`, the DCD and `cmake/usbx.cmake`:

1. **Host MPS** -- chunk the data-out at the device's enumerated endpoint
   `wMaxPacketSize` (FS = 64), not the host controller's speed ceiling (HS =
   512). `ra8_usb_host_bulk_out` ships one packet per call.
2. **Device DBLB** -- double-buffer the device bulk-OUT pipe so the host's next
   packet lands in bank B while the ISR drains bank A.
3. **Loop-drain** -- the DCD drains every ready OUT bank per interrupt.
4. **Single-chunk buffer** -- `UX_SLAVE_REQUEST_DATA_MAX_LENGTH` is large enough
   to keep a typical WRITE in one device transfer, with no inter-chunk gap.

## Pinout

FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW, P8_14/P8_15 data (PSEL
usb_fs). HS host: SW4-8 Host via the U15 expander, PD07 HIGH (U18 powers J7),
P4_08 VBUS sense (PSEL usb_hs). Console: PD_02/PD_03 SCI8. OSPI: xSPI CS1 to the
onboard IS25LX512M via the U15 octo-SPI mux. The device advertises VID 0x1209
with a per-app PID; bench use only.

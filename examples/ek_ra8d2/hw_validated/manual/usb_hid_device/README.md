# usb_hid_device (hw_pending)

USBX HID device demo (3-button boot mouse) for EK-RA8D2.

## Status (2026-05-19)

After fixing VBUSEN routing (now GPIO output LOW instead of
peripheral function), the chip enumerates on the Pi-side host as
VID 1209:0001:

```
usb 2-1.3.4: New USB device found, idVendor=1209, idProduct=0001
```

But the `usbhid` driver fails to bind:

```
usbhid 2-1.3.4:1.0: can't add hid device: -110
usbhid: probe of 2-1.3.4:1.0 failed with error -110
```

`-110 = ETIMEDOUT` -- the kernel times out on a subsequent
class-specific descriptor read (likely GET_HID_REPORT_DESCRIPTOR).
USBX is responding to the standard chapter-9 enumeration but
hanging when the host requests the HID Report Descriptor over EP0.

The hand-rolled `internal_wait_frdy` in `libs/ra8_hal/src/ra8_usb.c`
is the suspect: in `usb_cdc_echo` we found the same wait was the
final block before USBX could deliver multi-segment EP0 IN
transfers. The HID Report Descriptor (s_report_descriptor is 50+
bytes) gets chunked into multiple EP0 IN packets, and if the FRDY
wait times out between chunks the kernel sees -110.

Counters `g_usb_hid_match` / `g_usb_hid_mismatch` were added so the
next iteration can use `jlink_memprobe` HIL_MODE to detect when the
worker actually pushes HID events (will only advance once the host
opens /dev/hidraw*, which requires the report-descriptor handshake
to complete).

## How to graduate back

1. Trace the EP0 IN multi-chunk path in `libs/ra8_hal/src/ra8_usb.c`;
   compare against the Renesas FSP r_usb implementation of
   `internal_wait_frdy`. Specifically, after writing a 64-byte
   chunk into CFIFO, FRDY must clear ("port busy"), then the host
   reads the chunk and FRDY reasserts. The current wait loop may
   only check FRDY=1 once instead of the FRDY=0 -> FRDY=1
   transition the next chunk needs.
2. Once /dev/hidraw* appears, `usb_benchmark_hid.py` (to be
   written -- it's not in scripts/ yet) reads the report
   descriptor and round-trips a feature report.
3. Promote once `g_usb_hid_match` advances steadily under load.

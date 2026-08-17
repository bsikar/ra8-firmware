# usb_msc_device

USB Mass Storage smoke test on USB-FS. The board enumerates as a removable drive
backed by a blank RAM disk with no filesystem on it, so the host formats it; the
USBX device storage class serves the BOT/SCSI transport and the app supplies the
RAM-backed read, write and status hooks. It is the scaffold the MRAM storage
apps are built on.

It is manual because the proof is a human formatting the disk from a PC, writing
a file, unmounting, remounting, and finding the file still there.

## Linux tolerating a defect is not evidence of conformance

Getting macOS to accept the device took two first-party fixes that the Linux
path had happily ignored:

- The vendored USBX INQUIRY handler reports RESPONSE DATA FORMAT 0 (SCSI-1) and
  ignores the EVPD bit. macOS answers that with a Bulk-Only Mass Storage Reset
  and abandons the device. It is replaced by a first-party override under
  `port/usbx/` that reports SPC-2 and serves VPD pages 0x00 and 0x80.
- The device-controller bridge gained strand recovery for stashed bulk-IN
  transfers (a NAK PID with a bank already loaded) and a transactional BEMPSTS
  acknowledgement for multi-packet IN streaming. The JLink-readable BOT event
  trace ring added alongside is what made the failure visible at all.

The USB IDs come from the pid.codes free-for-experiments range and are bench-only
-- nothing here is a registered product. LED2 toggles as the class runs
(EK-RA8D2 v1 UM Table 24 p 31).

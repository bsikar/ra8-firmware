#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# usb_libusb_bench.py -- measure raw bulk-transfer throughput against a
# CDC-ACM echo device, BYPASSING the kernel's cdc_acm driver. This is
# the right tool for asking "what is the device firmware actually
# capable of, separate from cdc_acm overhead?"
#
# How it works:
#   1. Detach the cdc_acm kernel driver from the bulk interface.
#   2. Claim the data interface via libusb.
#   3. Submit asynchronous bulk-OUT transfers back-to-back and a pool of
#      bulk-IN transfers, both deep enough that the firmware never sees
#      the host idle.
#   4. Measure wall time for the whole round-trip.
#
# Re-attaches cdc_acm on exit (clean unmount), so the device shows up
# as /dev/ttyACMx again immediately afterwards.
#
# Usage:
#   sudo python3 usb_libusb_bench.py --vidpid 1209:000c [--bytes 1048576]

import argparse
import contextlib
import sys
import time

import usb.core
import usb.util

CDC_DATA_INTERFACE_CLASS = 0x0A  # USB CDC data class (bInterfaceClass)
USB_TRANSFER_TYPE_BULK = 0x02  # bmAttributes transfer-type field value
USB_TRANSFER_TYPE_MASK = 0x03  # mask to extract transfer type from bmAttributes
USB_EP_DIR_IN_BIT = 0x80  # bEndpointAddress direction bit: 1 = IN


def find_bulk_endpoints(dev):
    """Return (cfg, interface_num, ep_out_addr, ep_in_addr) for the first
    bulk-pair interface we find on `dev`. CDC ACM puts the bulk pair on
    the *data* interface (class 0x0a), not on the comm interface."""
    cfg = dev.get_active_configuration()
    for intf in cfg:
        if intf.bInterfaceClass != CDC_DATA_INTERFACE_CLASS:
            continue
        ep_out = ep_in = None
        for ep in intf:
            attr = ep.bmAttributes & USB_TRANSFER_TYPE_MASK
            if attr != USB_TRANSFER_TYPE_BULK:
                continue  # Not bulk
            if (ep.bEndpointAddress & USB_EP_DIR_IN_BIT) == 0:
                ep_out = ep.bEndpointAddress
            else:
                ep_in = ep.bEndpointAddress
        if ep_out is not None and ep_in is not None:
            return cfg.bConfigurationValue, intf.bInterfaceNumber, ep_out, ep_in
    return None


def bench(vidpid, total_bytes, urb_size, parallel_out, parallel_in):  # noqa: ARG001, PLR0912, PLR0915  # parallel_* reserved for future async batching; branchy dispatch
    vid_s, pid_s = vidpid.split(":")
    vid = int(vid_s, 16)
    pid = int(pid_s, 16)

    dev = usb.core.find(idVendor=vid, idProduct=pid)
    if dev is None:
        print(f"No device with vid:pid {vidpid}")
        sys.exit(2)
    print(
        f"Found {dev.manufacturer or '?'} / {dev.product or '?'} bus {dev.bus} addr {dev.address}"
    )

    info = find_bulk_endpoints(dev)
    if info is None:
        print("No CDC-data bulk endpoints found.")
        sys.exit(2)
    cfg_val, intf_num, ep_out, ep_in = info
    print(f"cfg={cfg_val} intf={intf_num} ep_out=0x{ep_out:02x} ep_in=0x{ep_in:02x}")

    # Detach cdc_acm if it's bound to this interface.
    for i in (intf_num - 1, intf_num):  # comm + data
        try:
            if dev.is_kernel_driver_active(i):
                print(f"Detaching kernel driver from interface {i}")
                dev.detach_kernel_driver(i)
        except (NotImplementedError, usb.core.USBError) as e:  # noqa: PERF203  # short 2-iter loop; inline try is clearest
            print(f"Kernel-driver detach on intf {i} ignored: {e}")

    try:
        usb.util.claim_interface(dev, intf_num)

        # Build a recognisable pseudo-random payload.
        payload = bytes((i * 131 + 17) & 0xFF for i in range(total_bytes))

        # Single-threaded synchronous IO with chunked write+read; rely on
        # libusb's per-transfer kernel buffering. urb_size is the chunk
        # we submit at a time. Larger = more pipelining.
        sent = 0
        recv = bytearray()
        start = time.monotonic()
        deadline = start + 30.0

        while sent < total_bytes or len(recv) < total_bytes:
            # Try to keep `parallel_out` bytes outstanding.
            if sent < total_bytes:
                chunk = payload[sent : sent + urb_size]
                try:
                    n = dev.write(ep_out, chunk, timeout=1000)
                    sent += n
                except usb.core.USBTimeoutError:
                    pass
            # Drain as much as available.
            try:
                got = dev.read(ep_in, urb_size, timeout=50)
                if got:
                    recv += bytes(got)
            except usb.core.USBTimeoutError:
                pass
            if time.monotonic() > deadline:
                print("TIMEOUT")
                break
        elapsed = time.monotonic() - start
        oneway = total_bytes / elapsed if elapsed > 0 else 0.0
        ok = bytes(recv) == payload[: len(recv)] and len(recv) == total_bytes

        print(f"sent={sent}  recv={len(recv)}  elapsed={elapsed:.3f}s")
        print(f"one-way throughput : {oneway / 1024:9.1f} KB/s  ({oneway / 1e6 * 8:.2f} Mbps)")
        print(
            f"aggregate          : {2 * oneway / 1024:9.1f} KB/s  ({2 * oneway / 1e6 * 8:.2f} Mbps)"
        )
        print(f"integrity          : {'OK' if ok else 'FAIL'}")
        if not ok:
            for i in range(min(len(recv), len(payload))):
                if recv[i] != payload[i]:
                    lo, hi = max(0, i - 8), min(len(recv), i + 16)
                    print(f"  first mismatch at offset {i}")
                    print(f"    expected: {payload[lo:hi].hex()}")
                    print(f"    received: {bytes(recv[lo:hi]).hex()}")
                    break
        sys.exit(0 if ok else 1)
    finally:
        with contextlib.suppress(Exception):
            usb.util.release_interface(dev, intf_num)
        # Re-attach the kernel driver so the device comes back as ttyACM.
        for i in (intf_num - 1, intf_num):
            with contextlib.suppress(Exception):
                dev.attach_kernel_driver(i)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vidpid", required=True, help="VID:PID, e.g. 1209:000c")
    ap.add_argument("--bytes", type=int, default=1048576)
    ap.add_argument("--urb", type=int, default=4096)
    ap.add_argument("--parallel-out", type=int, default=4)
    ap.add_argument("--parallel-in", type=int, default=4)
    args = ap.parse_args()
    bench(args.vidpid, args.bytes, args.urb, args.parallel_out, args.parallel_in)


if __name__ == "__main__":
    main()

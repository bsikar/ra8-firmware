#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Measure raw bulk throughput to a CDC-ACM device, bypassing cdc_acm.

The right tool for asking "what is the device firmware actually capable of,
separate from kernel driver overhead?". ``stream_bench.py`` measures the same
link through /dev/ttyACM*, so comparing the two attributes any shortfall to
cdc_acm rather than to the firmware.

How it works:
  1. Detach the cdc_acm kernel driver from the bulk interface.
  2. Claim the data interface via libusb.
  3. Write and read bulk transfers back-to-back so the firmware never sees
     the host idle.
  4. Measure wall time for the whole round-trip.

Re-attaches cdc_acm on exit via a finally block, so the device comes back as
/dev/ttyACMx even if the benchmark fails partway -- leaving it detached would
strand the port until replug.

Requires root, since detaching a kernel driver does.

Usage:
  sudo python3 libusb_bench.py --vidpid 1209:000c [--bytes 1048576]
"""

from __future__ import annotations

import argparse
import contextlib
import sys
import time
from typing import NoReturn

import usb.core
import usb.util

CDC_DATA_INTERFACE_CLASS = 0x0A  # USB CDC data class (bInterfaceClass)
USB_TRANSFER_TYPE_BULK = 0x02  # bmAttributes transfer-type field value
USB_TRANSFER_TYPE_MASK = 0x03  # mask to extract transfer type from bmAttributes
USB_EP_DIR_IN_BIT = 0x80  # bEndpointAddress direction bit: 1 = IN


def find_bulk_endpoints(dev: usb.core.Device) -> tuple[int, int, int, int] | None:
    """Locate the first interface carrying a bulk IN/OUT pair.

    Searches only CDC DATA interfaces (class 0x0A). That restriction is the
    point: CDC ACM splits the function across a comm interface and a data
    interface, and the bulk pair lives on the data one -- scanning every
    interface would match the comm interface's interrupt endpoint first.

    An interface offering only one direction is skipped rather than returned
    half-filled, since the echo bench needs both.

    Returns ``(cfg_value, interface_num, ep_out_addr, ep_in_addr)``, or None
    when the device exposes no such interface.
    """
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


def _open_device(vidpid: str) -> tuple[usb.core.Device, int, int, int]:
    """Find the device and its CDC-data bulk endpoints, or exit(2)."""
    vid_s, pid_s = vidpid.split(":")
    dev = usb.core.find(idVendor=int(vid_s, 16), idProduct=int(pid_s, 16))
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
    return dev, intf_num, ep_out, ep_in


def _detach_kernel_driver(dev: usb.core.Device, intf_num: int) -> None:
    """Unbind cdc_acm from the comm+data interface pair so libusb can claim it."""
    for i in (intf_num - 1, intf_num):
        try:
            if dev.is_kernel_driver_active(i):
                print(f"Detaching kernel driver from interface {i}")
                dev.detach_kernel_driver(i)
        except (NotImplementedError, usb.core.USBError) as e:  # noqa: PERF203  # short 2-iter loop; inline try is clearest
            print(f"Kernel-driver detach on intf {i} ignored: {e}")


def _pump(
    dev: usb.core.Device, ep_out: int, ep_in: int, payload: bytes, urb_size: int
) -> tuple[int, bytearray, float]:
    """Chunked write+read until both directions have moved the whole payload."""
    total_bytes = len(payload)
    sent = 0
    recv = bytearray()
    start = time.monotonic()
    deadline = start + 30.0
    while sent < total_bytes or len(recv) < total_bytes:
        if sent < total_bytes:
            with contextlib.suppress(usb.core.USBTimeoutError):
                sent += dev.write(ep_out, payload[sent : sent + urb_size], timeout=1000)
        with contextlib.suppress(usb.core.USBTimeoutError):
            got = dev.read(ep_in, urb_size, timeout=50)
            if got:
                recv += bytes(got)
        if time.monotonic() > deadline:
            print("TIMEOUT")
            break
    return sent, recv, time.monotonic() - start


def _report(sent: int, recv: bytes | bytearray, payload: bytes, elapsed: float) -> bool:
    """Print throughput and integrity; return True when the echo matched."""
    total_bytes = len(payload)
    oneway = total_bytes / elapsed if elapsed > 0 else 0.0
    ok = bytes(recv) == payload[: len(recv)] and len(recv) == total_bytes
    print(f"sent={sent}  recv={len(recv)}  elapsed={elapsed:.3f}s")
    print(f"one-way throughput : {oneway / 1024:9.1f} KB/s  ({oneway / 1e6 * 8:.2f} Mbps)")
    print(f"aggregate          : {2 * oneway / 1024:9.1f} KB/s  ({2 * oneway / 1e6 * 8:.2f} Mbps)")
    print(f"integrity          : {'OK' if ok else 'FAIL'}")
    if not ok:
        for i in range(min(len(recv), len(payload))):
            if recv[i] != payload[i]:
                lo, hi = max(0, i - 8), min(len(recv), i + 16)
                print(f"  first mismatch at offset {i}")
                print(f"    expected: {payload[lo:hi].hex()}")
                print(f"    received: {bytes(recv[lo:hi]).hex()}")
                break
    return ok


def bench(
    vidpid: str,
    total_bytes: int,
    urb_size: int,
    parallel_out: int,  # noqa: ARG001  # accepted, unused: reserved for async batching
    parallel_in: int,  # noqa: ARG001  # accepted, unused: reserved for async batching
) -> NoReturn:
    """Run one synchronous bulk echo benchmark over raw libusb, then exit.

    Never returns -- exits 0 when the echo matched, 1 when it did not.

    The kernel driver is re-attached in a finally block, so an exception or a
    failed run still leaves the device usable as /dev/ttyACMx; without that a
    crash here would strand the port until it was physically replugged.

    ``parallel_out`` / ``parallel_in`` are accepted and deliberately unused:
    the transfer loop is single-buffered today, and the parameters exist so
    the async variant can land without a CLI change.
    """
    dev, intf_num, ep_out, ep_in = _open_device(vidpid)
    _detach_kernel_driver(dev, intf_num)
    try:
        usb.util.claim_interface(dev, intf_num)
        payload = bytes((i * 131 + 17) & 0xFF for i in range(total_bytes))
        sent, recv, elapsed = _pump(dev, ep_out, ep_in, payload, urb_size)
        sys.exit(0 if _report(sent, recv, payload, elapsed) else 1)
    finally:
        with contextlib.suppress(Exception):
            usb.util.release_interface(dev, intf_num)
        # Re-attach the kernel driver so the device comes back as ttyACM.
        for i in (intf_num - 1, intf_num):
            with contextlib.suppress(Exception):
                dev.attach_kernel_driver(i)


def main() -> NoReturn:
    """Parse the command line and run one libusb bulk echo benchmark.

    Never returns: ``bench`` exits with the verdict (0 clean, 1 integrity
    failure, 2 device not found or no bulk endpoints).

    ``--parallel-out`` / ``--parallel-in`` are accepted and currently unused;
    the transfer loop is synchronous. They are kept so the async batching
    variant can land without changing this tool's command line.
    """
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

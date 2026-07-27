#!/usr/bin/env python3
"""Smoke-test the Digilent Analog Discovery 2 through the WaveForms SDK.

Answers one question: can this bench actually capture with the AD2? That needs
three separate things to be true, and this script fails distinctly on each so
the failure names its own fix:

1. ``libdwf`` loads. The WaveForms deb is installed by extracting it, not by
   apt (see the ``ad2_tools`` Ansible role for why), so a botched extract shows
   up here as a load error rather than as a missing package.
2. The SDK enumerates at least one device. Distinguishes "instrument unplugged
   or claimed by ``ftdi_sio``" from "software broken".
3. A device opens. This is the step that needs
   ``/usr/share/digilent/waveforms`` -- without those firmware/configuration
   resources ``FDwfDeviceOpen`` fails with "Device not supported. No compatible
   configuration found", which enumeration alone would never reveal.

A device that is already open in another process reports "busy", which counts
as a PASS: it proves the library, the device and the resources are all good,
and a bench running a live capture must not be flagged as broken.

Exit status is 0 on pass and 1 on any failure, so it is usable as a gate.
"""

from __future__ import annotations

import ctypes
import sys

# libdwf reports failure as 0 and success as non-zero (its own convention, not
# an errno), and hands back a device handle of 0 for "not opened".
DWF_FAILURE = 0
DWF_BAD_HANDLE = 0
# FDwfEnum device filter: 0 == enumfilterAll, every supported Digilent device.
DWF_ENUM_FILTER_ALL = 0
# FDwfGetLastErrorMsg writes into a caller-supplied buffer; the SDK documents
# 512 bytes as the required size.
DWF_ERROR_BUFFER_BYTES = 512
# Substring libdwf puts in the error message when the device is healthy but
# already claimed by another process. Matched case-insensitively.
DWF_BUSY_MARKER = "busy"


def load_dwf() -> ctypes.CDLL | None:
    """Load the WaveForms shared library.

    Returns:
        The loaded ``libdwf`` handle, or None when it cannot be loaded. The
        loader's own message and the remedy are printed to stderr in that case.
    """
    try:
        return ctypes.CDLL("libdwf.so")
    except OSError as exc:
        print(
            f"FAIL: cannot load libdwf.so: {exc}\n"
            "      The WaveForms SDK is not installed, or /usr/local/lib is not\n"
            "      in the loader cache. Re-run the ad2_tools Ansible role.",
            file=sys.stderr,
        )
        return None


def last_error(dwf: ctypes.CDLL) -> str:
    """Read the most recent libdwf error message.

    Args:
        dwf: The loaded ``libdwf`` handle.

    Returns:
        The SDK's error text, or a placeholder when it reported none.
    """
    buf = ctypes.create_string_buffer(DWF_ERROR_BUFFER_BYTES)
    dwf.FDwfGetLastErrorMsg(buf)
    return buf.value.decode(errors="replace").strip() or "(no message)"


def sdk_version(dwf: ctypes.CDLL) -> str:
    """Query the WaveForms SDK version string.

    Args:
        dwf: The loaded ``libdwf`` handle.

    Returns:
        The version reported by ``FDwfGetVersion``.
    """
    buf = ctypes.create_string_buffer(DWF_ERROR_BUFFER_BYTES)
    dwf.FDwfGetVersion(buf)
    return buf.value.decode(errors="replace").strip()


def enum_devices(dwf: ctypes.CDLL) -> int | None:
    """Count the Digilent devices the SDK can see.

    Args:
        dwf: The loaded ``libdwf`` handle.

    Returns:
        The number of enumerated devices, or None when the enumeration call
        itself failed (as opposed to succeeding with a count of zero).
    """
    count = ctypes.c_int()
    if dwf.FDwfEnum(ctypes.c_int(DWF_ENUM_FILTER_ALL), ctypes.byref(count)) == DWF_FAILURE:
        return None
    return count.value


def try_open(dwf: ctypes.CDLL) -> tuple[bool, str]:
    """Attempt to open the first available device and close it again.

    Args:
        dwf: The loaded ``libdwf`` handle.

    Returns:
        A ``(ok, detail)`` pair. ``ok`` is True when the device opened, or when
        it refused because another process holds it -- both prove the SDK, the
        device and the configuration resources are sound. ``detail`` is a
        human-readable outcome.
    """
    handle = ctypes.c_int()
    opened = dwf.FDwfDeviceOpen(ctypes.c_int(-1), ctypes.byref(handle))
    if opened != DWF_FAILURE and handle.value != DWF_BAD_HANDLE:
        dwf.FDwfDeviceClose(handle)
        return True, "opened and closed cleanly"

    detail = last_error(dwf)
    if DWF_BUSY_MARKER in detail.lower():
        return True, f"busy -- another process holds it ({detail})"
    return False, detail


def main() -> int:
    """Run the three-stage smoke test.

    Returns:
        0 when the bench can capture, 1 otherwise.
    """
    dwf = load_dwf()
    if dwf is None:
        return 1
    print(f"libdwf loaded, SDK version {sdk_version(dwf)}")

    count = enum_devices(dwf)
    if count is None:
        print(f"FAIL: FDwfEnum failed: {last_error(dwf)}", file=sys.stderr)
        return 1
    if count < 1:
        print(
            "FAIL: libdwf enumerates no devices.\n"
            "      Check the AD2 is plugged in and powered (lsusb -d 0403:6014),\n"
            "      and that the Adept runtime's udev rules ran dftdrvdtch to\n"
            "      detach ftdi_sio from the interface.",
            file=sys.stderr,
        )
        return 1
    print(f"libdwf enumerates {count} device(s)")

    ok, detail = try_open(dwf)
    if not ok:
        print(
            f"FAIL: FDwfDeviceOpen failed: {detail}\n"
            "      'No compatible configuration found' means the device\n"
            "      firmware/configuration resources are missing -- they must be\n"
            "      installed to /usr/share/digilent/waveforms. Re-run the\n"
            "      ad2_tools Ansible role.",
            file=sys.stderr,
        )
        return 1

    print(f"device open: {detail}")
    print("PASS: the AD2 is present and capture-capable")
    return 0


if __name__ == "__main__":
    sys.exit(main())

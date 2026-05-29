#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tapo_control.py -- Control the Tapo TP15 smart plug that powers the EK-RA8D2
# board on the HIL rig.  Credentials are read from <repo-root>/.env or, as a
# fallback, ~/.tapo.env.
#
# Usage:
#   python3 scripts/tapo_control.py [status|on|off|cycle]
#
# cycle powers the outlet off, waits 2 seconds, then powers it back on.

import asyncio
import os
import sys
from pathlib import Path

from dotenv import load_dotenv
from kasa import Credentials
from kasa.exceptions import KasaException
from kasa.deviceconfig import (
    DeviceConfig,
    DeviceConnectionParameters,
    DeviceEncryptionType,
    DeviceFamily,
)
from kasa.protocols.smartprotocol import SmartProtocol
from kasa.smart.smartdevice import SmartDevice
from kasa.transports.tpaptransport import TpapTransport

_REPO_ROOT = Path(__file__).resolve().parent.parent
_ENV_FILE = _REPO_ROOT / ".env"
_FALLBACK_ENV = Path.home() / ".tapo.env"

if _ENV_FILE.exists():
    load_dotenv(_ENV_FILE)
elif _FALLBACK_ENV.exists():
    load_dotenv(_FALLBACK_ENV)

TAPO_IP   = os.environ["TAPO_IP"]
TAPO_MAC  = os.environ["TAPO_MAC"]
TAPO_USER = os.environ["TAPO_USER"]
TAPO_PASS = os.environ["TAPO_PASS"]


async def _get_device() -> SmartDevice:
    creds = Credentials(TAPO_USER, TAPO_PASS)
    conn = DeviceConnectionParameters(
        DeviceFamily.SmartTapoPlug,
        DeviceEncryptionType.Tpap,
        login_version=2,
        https=False,
        http_port=80,
    )
    cfg = DeviceConfig(TAPO_IP, credentials=creds, connection_type=conn)
    t = TpapTransport(config=cfg)
    t._known_tpap_tls = 0
    t._known_tpap_port = 80
    t._known_tpap_dac = False
    t._known_tpap_pake = [2]
    t._known_tpap_user_hash_type = 0
    t._known_device_mac = TAPO_MAC
    es = t._encryption_session
    es._tpap_tls = 0
    es._tpap_port = 80
    es._tpap_dac = False
    es._tpap_pake = [2]
    es._tpap_user_hash_type = 0

    async def _noop() -> None:
        pass

    es._discover = _noop
    return SmartDevice(TAPO_IP, config=cfg, protocol=SmartProtocol(transport=t))


async def main() -> None:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    dev = await _get_device()
    try:
        await dev.update()
        if cmd == "on":
            await dev.turn_on()
            print(f"{dev.alias}: turned ON")
        elif cmd == "off":
            await dev.turn_off()
            print(f"{dev.alias}: turned OFF")
        elif cmd == "cycle":
            await dev.turn_off()
            print(f"{dev.alias}: OFF -- waiting 5 s...")
            await asyncio.sleep(5)
            await dev.turn_on()
            print(f"{dev.alias}: ON")
        else:
            state = "ON" if dev.is_on else "OFF"
            print(f"{dev.alias} ({dev.model}): {state}")
    except (KasaException, OSError, asyncio.TimeoutError) as exc:
        # Almost always a reachability problem (wrong VLAN/subnet, plug
        # offline, or firmware/kasa protocol drift) rather than a logic
        # bug -- surface a clean one-liner instead of a raw traceback.
        print(
            f"tapo_control: could not reach the plug at {TAPO_IP} "
            f"({type(exc).__name__}: {exc}). Check the Pi and plug are on "
            f"the same network and the plug is powered.",
            file=sys.stderr,
        )
        raise SystemExit(2) from exc
    finally:
        # Best-effort close; the client may not exist if setup failed early.
        try:
            await dev.protocol._transport._http_client.client.close()
        except Exception:  # noqa: BLE001 - cleanup must never mask the real error
            pass


asyncio.run(main())

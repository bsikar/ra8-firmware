#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Control a Tapo smart plug on the HIL rig.

This tooling drives three independently switched plugs:

  board -- powers the EK-RA8D2 target board.
  pi    -- powers the Raspberry Pi HIL host itself.  Driven directly from the
           developer workstation so the Pi can be power-cycled when wedged.
  relay -- powers the cam-relay homelab node (a Raspberry Pi 4B), which is NOT
           RA8 bench hardware.  Driven directly from the workstation, like pi;
           reads TAPO_RELAY_IP / TAPO_RELAY_MAC.

Recent TP15 firmware (>= 1.4) speaks TP-Link's TPAP protocol, which is not
UDP-discoverable; the connection parameters are therefore pinned explicitly
and discovery is skipped, so a plug is reached by address alone.  That is why
this module reaches into python-kasa internals -- the library exposes no
public seam for either -- and why it carries the tree's only SLF001 per-file
ignore.

Credentials and per-plug addresses are resolved by hil_secrets.populate_env(),
which prefers a self-hosted OpenBao vault and falls back to <repo-root>/.env
(or ~/.tapo.env) so the plug stays controllable even when OpenBao or the k3s
cluster is down.  The resolved values land in the environment as:

  TAPO_USER, TAPO_PASS          -- shared Tapo account
  TAPO_BOARD_IP, TAPO_BOARD_MAC -- board plug
  TAPO_PI_IP,    TAPO_PI_MAC    -- Pi plug
  TAPO_RELAY_IP, TAPO_RELAY_MAC -- cam-relay plug (.env only; see hil_secrets.py)

OpenBao consumer credentials (BAO_ADDR, ROLE_ID, SECRET_ID) live outside the
repo in ~/.config/hil/openbao.env; see scripts/hil/hil_secrets.py for details.

Usage:
  python3 scripts/hil/tapo_control.py <board|pi|relay> [status|on|off|cycle]

cycle powers the outlet off, waits 5 seconds, then powers it back on.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import sys
from pathlib import Path

import hil_secrets
from kasa import Credentials
from kasa.deviceconfig import (
    DeviceConfig,
    DeviceConnectionParameters,
    DeviceEncryptionType,
    DeviceFamily,
)
from kasa.exceptions import KasaException
from kasa.protocols.smartprotocol import SmartProtocol
from kasa.smart.smartdevice import SmartDevice
from kasa.transports.tpaptransport import TpapTransport

_REPO_ROOT = Path(__file__).resolve().parents[2]
_ENV_FILE = _REPO_ROOT / ".env"
_FALLBACK_ENV = Path.home() / ".tapo.env"

# Resolve secrets: OpenBao first, then the local .env fallback.  The source is
# reported on stderr so it is clear which path supplied the credentials.
_SECRET_SOURCE = hil_secrets.populate_env(_ENV_FILE, _FALLBACK_ENV)
print(f"tapo_control: secrets source = {_SECRET_SOURCE}", file=sys.stderr)

_TARGETS = ("board", "pi", "relay")
_COMMANDS = ("status", "on", "off", "cycle")
_OFF_SECONDS = 5
_HTTP_PORT = 80
_ARGV_TARGET = 1
_ARGV_CMD = 2


def _usage() -> None:
    print(
        "usage: tapo_control.py <board|pi|relay> [status|on|off|cycle]",
        file=sys.stderr,
    )
    raise SystemExit(2)


def _require(name: str) -> str:
    val = os.environ.get(name, "").strip()
    if not val:
        print(
            f"tapo_control: missing {name} -- set it in .env "
            f"(copy .env.example and fill in values).",
            file=sys.stderr,
        )
        raise SystemExit(2)
    return val


async def _get_device(ip: str, mac: str, creds: Credentials) -> SmartDevice:
    # TPAP is not discoverable -- pin every connection parameter and stub out
    # discovery so the device is reached by address alone.
    conn = DeviceConnectionParameters(
        DeviceFamily.SmartTapoPlug,
        DeviceEncryptionType.Tpap,
        login_version=2,
        https=False,
        http_port=_HTTP_PORT,
    )
    cfg = DeviceConfig(ip, credentials=creds, connection_type=conn)
    t = TpapTransport(config=cfg)
    t._known_tpap_tls = 0
    t._known_tpap_port = _HTTP_PORT
    t._known_tpap_dac = False
    t._known_tpap_pake = [2]
    t._known_tpap_user_hash_type = 0
    t._known_device_mac = mac
    es = t._encryption_session
    es._tpap_tls = 0
    es._tpap_port = _HTTP_PORT
    es._tpap_dac = False
    es._tpap_pake = [2]
    es._tpap_user_hash_type = 0

    async def _noop() -> None:
        pass

    es._discover = _noop
    return SmartDevice(ip, config=cfg, protocol=SmartProtocol(transport=t))


async def main() -> None:
    """Run one plug command: status (the default), on, off, or cycle.

    Both the target and the command are validated against fixed sets before
    any network call, so a typo prints usage instead of reaching a plug --
    which matters when the targets include "the machine running the HIL suite"
    and "a live homelab node", where the wrong one cuts power to something you
    did not mean to touch.

    Every required credential is resolved through ``_require``, so a missing
    environment variable fails immediately and by name rather than surfacing
    later as an authentication error against the plug.
    """
    if len(sys.argv) <= _ARGV_TARGET or sys.argv[_ARGV_TARGET] not in _TARGETS:
        _usage()
    target = sys.argv[_ARGV_TARGET]
    cmd = sys.argv[_ARGV_CMD] if len(sys.argv) > _ARGV_CMD else "status"
    if cmd not in _COMMANDS:
        _usage()

    ip = _require(f"TAPO_{target.upper()}_IP")
    mac = _require(f"TAPO_{target.upper()}_MAC")
    creds = Credentials(_require("TAPO_USER"), _require("TAPO_PASS"))

    dev = await _get_device(ip, mac, creds)
    try:
        await dev.update()
        if cmd == "on":
            await dev.turn_on()
            print(f"{target} ({dev.alias}): turned ON")
        elif cmd == "off":
            await dev.turn_off()
            print(f"{target} ({dev.alias}): turned OFF")
        elif cmd == "cycle":
            await dev.turn_off()
            print(f"{target} ({dev.alias}): OFF -- waiting {_OFF_SECONDS} s...")
            await asyncio.sleep(_OFF_SECONDS)
            await dev.turn_on()
            print(f"{target} ({dev.alias}): ON")
        else:
            state = "ON" if dev.is_on else "OFF"
            print(f"{target} ({dev.alias}, {dev.model}): {state}")
    except (KasaException, OSError, asyncio.TimeoutError) as exc:
        # Almost always reachability or wrong credentials rather than a logic
        # bug -- surface a clean one-liner instead of a raw traceback.
        print(
            f"tapo_control: could not control the {target} plug at {ip} "
            f"({type(exc).__name__}: {exc}). Check the plug is powered, on a "
            f"network this machine can reach, and TAPO_USER/TAPO_PASS are correct.",
            file=sys.stderr,
        )
        raise SystemExit(2) from exc
    finally:
        with contextlib.suppress(Exception):
            await dev.protocol._transport._http_client.client.close()


asyncio.run(main())

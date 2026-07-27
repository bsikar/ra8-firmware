#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# verify_bench_wifi.sh -- end-to-end proof the bench LAN works, WITHOUT the
# ESP32-C6. From the bench Pi it joins the `ra8-bench` SSID on its onboard
# wlan0, confirms a DHCP lease on 10.0.40.0/24, pings the FortiGate (10.0.40.1)
# and the AP (10.0.40.10), then restores wlan0 to its prior connection.
#
# SAFETY: star's ONLY uplink (and its Tailscale path) rides wlan0. Joining the
# isolated, internet-less bench SSID cuts that path. So this script arms a
# GUARANTEED auto-restore BEFORE it disconnects: a detached `at`/background job
# re-selects the original connection after RESTORE_AFTER seconds no matter what
# happens to this script (crash, ssh drop, timeout). The normal path restores
# immediately and cancels the timer.
#
# The SSID passphrase is read from OpenBao (secret/ra8d2/bench-network); it is
# never placed on the command line or in a log.
#
# Usage:
#   infra/network/verify_bench_wifi.sh            # run the join/ping/restore
#   RESTORE_AFTER=180 infra/network/verify_bench_wifi.sh
#
# STATUS: `ra8-bench` is live (2026-07-27), but this test needs the runner to be
# in RF range of the AP. The bench Pi's onboard wlan0 is out of range of the
# ceiling-mounted MR18, so run this from a client that can hear the AP (or cable
# star's RTL8153 onto a FortiGate LAN port for a wired lease/ping proof). See
# README.md "Current status".
set -euo pipefail

IFACE="wlan0"
SSID="ra8-bench"
FW_IP="10.0.40.1"
AP_IP="10.0.40.10"
RESTORE_AFTER="${RESTORE_AFTER:-180}"

bench_psk() {
  python3 - <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, str(Path.home() / "ra8-firmware" / "scripts" / "secrets"))
from openbao_client import OpenBaoClient, load_config

sys.stdout.write(OpenBaoClient(load_config()).kv_get("ra8d2/bench-network")["bench_psk"])
PY
}

# The connection wlan0 is on right now -- restore target.
PRIOR_CON="$(nmcli -t -g GENERAL.CONNECTION dev show "${IFACE}" || true)"
if [ -z "${PRIOR_CON}" ]; then
  echo "refusing to run: ${IFACE} has no active connection to restore" >&2
  exit 1
fi
echo "prior ${IFACE} connection: ${PRIOR_CON}" >&2

# Arm the dead-man's-switch restore. Detached from this shell's lifetime.
RESTORE_CMD="nmcli con up '${PRIOR_CON}' ifname ${IFACE}"
if command -v at >/dev/null 2>&1; then
  echo "${RESTORE_CMD}" | at now + "$((RESTORE_AFTER / 60 + 1))" minutes 2>/dev/null || true
fi
setsid bash -c "sleep ${RESTORE_AFTER}; ${RESTORE_CMD}" >/dev/null 2>&1 </dev/null &
GUARD_PID=$!
echo "auto-restore armed (pid ${GUARD_PID}) -> '${PRIOR_CON}' in ${RESTORE_AFTER}s" >&2

# shellcheck disable=SC2317,SC2329  # invoked indirectly via the EXIT trap below
restore_now() {
  kill "${GUARD_PID}" 2>/dev/null || true
  nmcli con up "${PRIOR_CON}" ifname "${IFACE}" >/dev/null 2>&1 || true
  echo "restored ${IFACE} -> ${PRIOR_CON}" >&2
}
trap restore_now EXIT

RESULT=1
{
  PSK="$(bench_psk)"
  nmcli dev wifi connect "${SSID}" password "${PSK}" ifname "${IFACE}"
  unset PSK
}
sleep 5

LEASE="$(nmcli -t -g IP4.ADDRESS dev show "${IFACE}" | head -1 || true)"
echo "lease on ${IFACE}: ${LEASE}" >&2
case "${LEASE}" in
  10.0.40.*) echo "PASS: DHCP lease is on 10.0.40.0/24" >&2 ;;
  *)
    echo "FAIL: no 10.0.40.x lease (got '${LEASE}')" >&2
    exit "${RESULT}"
    ;;
esac

if ping -c 3 -W 2 "${FW_IP}" && ping -c 3 -W 2 "${AP_IP}"; then
  echo "PASS: reached FortiGate ${FW_IP} and AP ${AP_IP}" >&2
  RESULT=0
else
  echo "FAIL: could not ping both ${FW_IP} and ${AP_IP}" >&2
fi

exit "${RESULT}"

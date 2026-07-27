#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ap_openwrt.sh -- configure the Meraki MR18 (OpenWrt) as the bench access
# point for the isolated ESP32-C6 wireless LAN.
#
# It reads every secret from OpenBao (secret/ra8d2/bench-network) and pushes a
# uci batch to the AP over ssh. No password or PSK is ever written to a repo
# file, an argument list, or a log. The uci batch is fed to the AP on stdin so
# the PSK never appears in the remote process table either.
#
# The MR18 is a single-band-per-radio ath9k device: radio0 = 2.4 GHz, radio1 =
# 5 GHz (confirm with `iwinfo` on the box). The ESP32-C6 is 2.4 GHz-only, so
# the bench SSID is pinned to radio0. Everything bridges to a single flat
# br-lan (no VLAN tags) to match the wiped, flat 10.0.40.0/24 FortiGate LAN.
#
# Usage:
#   infra/network/ap_openwrt.sh apply      # push the config (default)
#   infra/network/ap_openwrt.sh show       # dump the AP's live wireless/network
#   infra/network/ap_openwrt.sh dryrun     # print the uci batch, connect to nothing
#
# Prereqs: run from the bench Pi or the Mac with a working path to the AP
# (10.0.40.10) and the OpenBao consumer creds at ~/.config/hil/openbao.env.
#
# STATUS: NOT yet applied. There is currently no network path to the AP: it is
# PoE-powered by the (un-wiped, locked-out) FortiGate, and no legacy SSID is
# broadcasting for an over-the-air path. See README.md "Current status".
set -euo pipefail

MODE="${1:-apply}"

# Resolve one field of the bench-network secret from OpenBao (read-only).
bao_field() {
  python3 - "$1" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, str(Path.home() / "ra8-firmware" / "scripts" / "secrets"))
from openbao_client import OpenBaoClient, load_config

data = OpenBaoClient(load_config()).kv_get("ra8d2/bench-network")
sys.stdout.write(data[sys.argv[1]])
PY
}

# Emit the uci batch on stdout. Reads the PSK from stdin (fd 3) so it is not a
# shell variable that could leak via `set -x` or the environment.
render_uci() {
  local ssid="$1" ap_ip="$2"
  cat <<UCI
set network.lan.ipaddr='${ap_ip}'
set network.lan.netmask='255.255.255.0'
set network.lan.gateway='10.0.40.1'
set network.lan.dns='10.0.40.1'
set network.lan.proto='static'

set wireless.radio0.disabled='0'
set wireless.radio0.country='US'
set wireless.radio0.channel='6'
set wireless.radio0.htmode='HT20'

delete wireless.bench 2>/dev/null || true
set wireless.bench='wifi-iface'
set wireless.bench.device='radio0'
set wireless.bench.mode='ap'
set wireless.bench.network='lan'
set wireless.bench.ssid='${ssid}'
set wireless.bench.encryption='psk2'
set wireless.bench.key='__PSK__'
set wireless.bench.disabled='0'
UCI
}

case "${MODE}" in
  show)
    AP_IP="$(bao_field ap_ip)"
    AP_USER="$(bao_field ap_ssh_user)"
    echo "== ${AP_USER}@${AP_IP}: wireless + network ==" >&2
    exec ssh "${AP_USER}@${AP_IP}" 'uci show wireless; echo ---; uci show network; echo ---; iwinfo'
    ;;
  dryrun)
    SSID="$(bao_field bench_ssid)"
    AP_IP="$(bao_field ap_ip)"
    render_uci "${SSID}" "${AP_IP}"
    echo "# (PSK redacted in dryrun; real run substitutes it from OpenBao)" >&2
    exit 0
    ;;
  apply) ;;
  *)
    echo "usage: ap_openwrt.sh [apply|show|dryrun]" >&2
    exit 2
    ;;
esac

SSID="$(bao_field bench_ssid)"
AP_IP="$(bao_field ap_ip)"
AP_USER="$(bao_field ap_ssh_user)"

# Build the batch with the PSK substituted, and stream it straight into
# `uci batch` on the AP via stdin -- never a file, never an argv.
{
  PSK="$(bao_field bench_psk)"
  render_uci "${SSID}" "${AP_IP}" | sed "s|__PSK__|${PSK}|"
  unset PSK
} | ssh "${AP_USER}@${AP_IP}" 'uci batch && uci commit && wifi reload && /etc/init.d/network reload'

echo "AP configured: SSID='${SSID}' on radio0 (2.4 GHz), static ${AP_IP}, bridged to flat br-lan." >&2

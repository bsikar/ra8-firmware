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
# The MR18 is a single-band-per-radio ath9k device. CONFIRMED on this unit:
# radio0 = 5 GHz, radio1 = 2.4 GHz. The ESP32-C6 is 2.4 GHz-only, so the bench
# SSID is pinned to radio1. The lan bridge is 'br-trusted' (untagged) to the
# FortiGate lan switch. This makes the AP a DUMB AP: it disables its own
# dnsmasq/odhcpd DHCP so the FortiGate (10.0.40.1) is the ONLY DHCP server, and
# it disables the orphaned iot/guest SSIDs whose VLAN networks were wiped from
# the FortiGate, keeping the working home-network on 'lan'.
#
# NOTE: with no direct IP route to 10.0.40.10, the AP is normally configured
# over the FortiGate console jump host -- see fg_bringup.py 'ap-configure',
# which applies this exact uci. This standalone script is for the case where a
# host (e.g. star's RTL8153) is cabled directly onto the bench LAN.
#
# Usage:
#   infra/network/ap_openwrt.sh apply      # push the config (default)
#   infra/network/ap_openwrt.sh show       # dump the AP's live wireless/network
#   infra/network/ap_openwrt.sh dryrun     # print the uci batch, connect to nothing
#
# Prereqs: run from the bench Pi or the Mac with a working path to the AP
# (10.0.40.10) and the OpenBao consumer creds at ~/.config/hil/openbao.env.
#
# STATUS: the equivalent uci has been APPLIED (2026-07-27) via the FortiGate
# console jump (fg_bringup.py ap-configure); ra8-bench is live on radio1. This
# standalone variant stays for the direct-IP-path case. See README.md.
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
set network.lan.proto='static'

set wireless.radio1.disabled='0'
set wireless.radio1.channel='6'
set wireless.radio1.htmode='HT20'

delete wireless.bench
set wireless.bench='wifi-iface'
set wireless.bench.device='radio1'
set wireless.bench.mode='ap'
set wireless.bench.network='lan'
set wireless.bench.ssid='${ssid}'
set wireless.bench.encryption='psk2'
set wireless.bench.key='__PSK__'
set wireless.bench.disabled='0'

set wireless.iot_5g.disabled='1'
set wireless.iot_2g.disabled='1'
set wireless.guest_5g.disabled='1'
set wireless.guest_2g.disabled='1'

set dhcp.lan.ignore='1'
set dhcp.lan.dhcpv4='disabled'
set dhcp.lan.dhcpv6='disabled'
set dhcp.lan.ra='disabled'
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
} | ssh "${AP_USER}@${AP_IP}" \
  'uci batch && uci commit && wifi reload && /etc/init.d/dnsmasq restart && /etc/init.d/odhcpd restart'

echo "AP configured: SSID='${SSID}' on radio1 (2.4 GHz), static ${AP_IP} on br-trusted, dumb-AP DHCP off." >&2

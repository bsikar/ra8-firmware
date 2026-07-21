#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_eth_tcp.sh -- Flash an Ethernet firmware app and verify that it
# answers on the wire. Supports three transports:
#
#   tcp   -- open a TCP socket to <board_ip>:<port>, send random N
#            bytes, read N bytes back, assert byte-exact equality.
#            (threadx_netx_tcp_echo)
#
#   udp   -- send a random N-byte UDP datagram to <board_ip>:<port>,
#            wait for a single reply, assert byte-exact equality.
#
#   http  -- issue an HTTP/1.1 GET to <board_ip>:<port>/, expect a
#            200 OK and assert the response body contains the marker
#            substring "Hello from RA8D2".
#
# Operates analogously to scripts/hil/run_direct.sh: when invoked off
# the HIL Pi it SCPs the firmware over and re-exec's itself on the Pi.
# All wire-side probing is done on the Pi (the host with the
# USB-Ethernet adapter wired to the EVM).
#
# The Pi-side USB-Ethernet interface is detected automatically -- any
# enxXX or usbX device with a 'lower-up' carrier and no IPv4 address.
# The helper temporarily assigns 192.168.1.1/24 to that interface for
# the duration of the test, then restores the prior state (no carrier,
# no address) on exit.
#
# Usage:
#   scripts/hil/eth_tcp.sh --hex <path> --board-ip <a.b.c.d> \
#                          --port <n>   --proto tcp|udp|http \
#                          [--payload-bytes 512] \
#                          [--boot-timeout 25] \
#                          [--probe-timeout 10]
#
# Exit codes:
#   0  PASS  -- byte-exact round-trip (TCP/UDP) or HTTP marker present
#   1  FAIL  -- mismatch, timeout, or HTTP marker missing
#   2  ERROR -- usage error or Pi unreachable

set -euo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN
PI_IP="192.168.1.1"
PI_PREFIX="24"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
  echo "Usage: $0 --hex <file> --board-ip <ip> --port <n> --proto tcp|udp|http"
  echo "          [--payload-bytes N] [--boot-timeout S] [--probe-timeout S]"
  exit 2
}

HEX=""
BOARD_IP=""
PORT=""
PROTO=""
PAYLOAD_BYTES=512
BOOT_TIMEOUT_S=25
PROBE_TIMEOUT_S=10
UART="/dev/ttyACM0"
BAUD=115200

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hex)
      HEX="$2"
      shift 2
      ;;
    --board-ip)
      BOARD_IP="$2"
      shift 2
      ;;
    --port)
      PORT="$2"
      shift 2
      ;;
    --proto)
      PROTO="$2"
      shift 2
      ;;
    --payload-bytes)
      PAYLOAD_BYTES="$2"
      shift 2
      ;;
    --boot-timeout)
      BOOT_TIMEOUT_S="$2"
      shift 2
      ;;
    --probe-timeout)
      PROBE_TIMEOUT_S="$2"
      shift 2
      ;;
    --uart)
      UART="$2"
      shift 2
      ;;
    --baud)
      BAUD="$2"
      shift 2
      ;;
    -h | --help)
      sed -n '4,45p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1"
      usage
      ;;
  esac
done

[[ -z "$HEX" || -z "$BOARD_IP" || -z "$PORT" || -z "$PROTO" ]] && usage
[[ -f "$HEX" ]] || {
  echo -e "${RED}[HIL]${NC} hex not found: $HEX"
  exit 2
}
case "$PROTO" in tcp | udp | http) ;; *)
  echo "bad --proto $PROTO"
  usage
  ;;
esac

APP_NAME="$(basename "${HEX%.hex}")"

LOCAL_PI=0
if rig_is_local_pi; then
  LOCAL_PI=1
fi

# Off-Pi: scp the hex over, re-invoke ourselves on the Pi.
if ((LOCAL_PI == 0)); then
  ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
    {
      echo -e "${RED}[HIL]${NC} cannot reach ${PI_HOST}"
      exit 2
    }
  REMOTE_HEX="/tmp/$(basename "$HEX")"
  scp -q "$HEX" "${PI_HOST}:${REMOTE_HEX}"
  if [[ -f "${HEX%.hex}.elf" ]]; then
    scp -q "${HEX%.hex}.elf" "${PI_HOST}:${REMOTE_HEX%.hex}.elf"
  fi
  # shellcheck disable=SC2029  # every value is local rig/app config passed as args to the piped script.
  ssh "$PI_HOST" "bash -s -- --hex '${REMOTE_HEX}' --board-ip '${BOARD_IP}' --port '${PORT}' --proto '${PROTO}' --payload-bytes '${PAYLOAD_BYTES}' --boot-timeout '${BOOT_TIMEOUT_S}' --probe-timeout '${PROBE_TIMEOUT_S}' --uart '${UART}' --baud '${BAUD}'" <"$0"
  exit $?
fi

echo -e "${YELLOW}[HIL]${NC} app=${APP_NAME}  proto=${PROTO}  board=${BOARD_IP}:${PORT}"

# ---- 1. Strip OFS sections ---------------------------------------------------
ELF="${HEX%.hex}.elf"
STRIPPED_HEX="/tmp/hil_${APP_NAME}_mram.hex"
OFS_ARGS=('--remove-section=.option_setting*')
if [[ -f "$ELF" ]]; then
  arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_HEX"
else
  arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_HEX" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_HEX"
fi

# ---- 2. Pre-arm the USB-Ethernet adapter ------------------------------------
# Find the USB-Ethernet interface (enx* or usb*). It will likely be in
# NO-CARRIER state until the firmware boots and the PHY negotiates.
USB_ETH_IFACE="$(ip -o link show 2>/dev/null |
  awk -F': ' '/enx|usb[0-9]+/ && !/lo/ {print $2; exit}' || true)"
if [[ -z "$USB_ETH_IFACE" ]]; then
  echo -e "${RED}[HIL]${NC} no USB-Ethernet interface (enx*/usb*) found on Pi" >&2
  exit 1
fi
echo -e "${YELLOW}[HIL]${NC} USB-Ethernet iface = ${USB_ETH_IFACE}"

# shellcheck disable=SC2329  # invoked from cleanup(), itself a trap handler.
restore_iface() {
  # Remove our temporary address but leave anything we did not set.
  sudo -n ip addr del "${PI_IP}/${PI_PREFIX}" dev "$USB_ETH_IFACE" 2>/dev/null || true
}
# shellcheck disable=SC2329  # invoked by `trap cleanup EXIT` below.
cleanup() {
  rm -f "$STRIPPED_HEX" "/tmp/hil_eth_jlink_${APP_NAME}.cmd"
  pkill -f "cat ${UART}" 2>/dev/null || true
  restore_iface
}
trap cleanup EXIT

# Assign 192.168.1.1/24 to the iface (idempotent).
if ! ip -4 addr show dev "$USB_ETH_IFACE" | grep -q "${PI_IP}/${PI_PREFIX}"; then
  sudo -n ip addr add "${PI_IP}/${PI_PREFIX}" dev "$USB_ETH_IFACE" 2>/dev/null ||
    {
      echo -e "${RED}[HIL]${NC} could not assign ${PI_IP} to ${USB_ETH_IFACE} (need sudo NOPASSWD on ip)"
      exit 1
    }
fi
sudo -n ip link set "$USB_ETH_IFACE" up 2>/dev/null || true

# ---- 3. Pre-flash LPSCR clear + flash ---------------------------------------
TMP_SCRIPT="/tmp/hil_eth_jlink_${APP_NAME}.cmd"
cat >"$TMP_SCRIPT" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
w2 0x4001E3FA 0xA502
w1 0x4001EA90 0x00
w2 0x4001E3FA 0xA500
loadfile ${STRIPPED_HEX}
r
g
q
JLINK

# Find JLink's VCOM /dev/ttyACMx.
JLINK_VID="1366"
JLINK_TTY=""
for _i in 1 2 3 4 5 6 7 8 9 10; do
  for d in /dev/ttyACM*; do
    [ -e "$d" ] || continue
    if udevadm info "$d" 2>/dev/null | grep -q "ID_VENDOR_ID=${JLINK_VID}"; then
      JLINK_TTY="$d"
      break 2
    fi
  done
  sleep 0.5
done
if [ -z "$JLINK_TTY" ]; then
  echo -e "${RED}[HIL]${NC} J-Link VCOM not found under /dev/ttyACM*" >&2
  exit 1
fi
UART="$JLINK_TTY"
stty -F "${UART}" "${BAUD}" raw -echo

UART_LOG="/tmp/hil_eth_uart_${APP_NAME}.log"
: >"${UART_LOG}"
pkill -f "cat ${UART}" 2>/dev/null || true
dd if="${UART}" iflag=nonblock of=/dev/null count=4 bs=1024 2>/dev/null || true
setsid stdbuf -o0 cat "${UART}" >"${UART_LOG}" 2>/dev/null &
READER_PID=$!
sleep 0.2

JLINK_LOG="/tmp/hil_eth_jlink_${APP_NAME}.log"
echo -e "${YELLOW}[HIL]${NC} flashing ${HEX}..."
JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$TMP_SCRIPT" \
  >"${JLINK_LOG}" 2>&1
if grep -qE "\*\*\*\*\*\* Error|Cannot connect to the probe|could not be halted|RAMCode did not respond" "${JLINK_LOG}"; then
  kill "${READER_PID}" 2>/dev/null
  echo -e "${RED}[HIL]${NC} J-Link error -- log tail:" >&2
  tail -20 "${JLINK_LOG}" >&2
  exit 1
fi
if ! grep -qE "Programming flash.*Done\.|Skipped\. Contents already match" "${JLINK_LOG}"; then
  kill "${READER_PID}" 2>/dev/null
  echo -e "${RED}[HIL]${NC} flash phase missing -- log tail:" >&2
  tail -20 "${JLINK_LOG}" >&2
  exit 1
fi
echo -e "${YELLOW}[HIL]${NC} flash OK"

# ---- 4. Wait for "eth: ready" on UART ---------------------------------------
echo -e "${YELLOW}[HIL]${NC} waiting up to ${BOOT_TIMEOUT_S}s for 'eth: ready'..."
deadline=$((SECONDS + BOOT_TIMEOUT_S))
READY=0
while ((SECONDS < deadline)); do
  if grep -qF "eth: ready" "${UART_LOG}" 2>/dev/null; then
    READY=1
    break
  fi
  sleep 0.2
done
echo "--- captured UART ---"
sed 's/\r/\\r/g' "${UART_LOG}" | head -30 | sed 's/^/[uart] /'
echo "--- end ---"
kill "${READER_PID}" 2>/dev/null || true
pkill -f "cat ${UART}" 2>/dev/null || true

if ((READY == 0)); then
  echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: never saw 'eth: ready' within ${BOOT_TIMEOUT_S}s"
  exit 1
fi

# ---- 5. Wait for PHY carrier + ARP-reachable -------------------------------
# After the board prints "eth: ready" the PHY may take another second to
# negotiate. We arping at the board IP and wait for a reply.
echo -e "${YELLOW}[HIL]${NC} probing carrier on ${USB_ETH_IFACE}..."
carrier_deadline=$((SECONDS + 10))
CARRIER_OK=0
while ((SECONDS < carrier_deadline)); do
  if ip link show "$USB_ETH_IFACE" | grep -q 'state UP\|LOWER_UP'; then
    CARRIER_OK=1
    break
  fi
  sleep 0.3
done
if ((CARRIER_OK == 0)); then
  echo -e "${YELLOW}[HIL]${NC} warn: no carrier on ${USB_ETH_IFACE} after 10s; proceeding anyway"
fi

echo -e "${YELLOW}[HIL]${NC} pinging ${BOARD_IP} via ${USB_ETH_IFACE} (timeout ${PROBE_TIMEOUT_S}s)..."
# Prefer arping (faster + uses the right iface). Fall back to ping -I if
# arping is not installed. Either way we poll until a reply, since the
# board's PHY may still be negotiating after "eth: ready" prints.
PING_OK=0
ping_deadline=$((SECONDS + PROBE_TIMEOUT_S))
if command -v arping >/dev/null 2>&1; then
  while ((SECONDS < ping_deadline)); do
    if arping -c 1 -w 1 -I "$USB_ETH_IFACE" "$BOARD_IP" >/dev/null 2>&1; then
      PING_OK=1
      break
    fi
    sleep 0.5
  done
else
  while ((SECONDS < ping_deadline)); do
    if ping -c 1 -W 1 -I "$USB_ETH_IFACE" "$BOARD_IP" >/dev/null 2>&1; then
      PING_OK=1
      break
    fi
    sleep 0.5
  done
fi
if ((PING_OK == 0)); then
  echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: no ICMP/ARP reply from ${BOARD_IP} within ${PROBE_TIMEOUT_S}s"
  echo "    iface=${USB_ETH_IFACE} pi_ip=${PI_IP}/${PI_PREFIX}"
  ip -o addr show dev "$USB_ETH_IFACE" 2>&1 | sed 's/^/    /'
  exit 1
fi
echo -e "${YELLOW}[HIL]${NC} ${BOARD_IP} reachable"

# ---- 6. Run the probe ------------------------------------------------------
# Use a small inline python helper -- bash + nc gets twitchy on the
# UDP / HTTP variants.
RESULT="$(
  PROTO="$PROTO" BOARD_IP="$BOARD_IP" PORT="$PORT" \
    PAYLOAD_BYTES="$PAYLOAD_BYTES" PROBE_TIMEOUT_S="$PROBE_TIMEOUT_S" \
    USB_ETH_IFACE="$USB_ETH_IFACE" \
    python3 - <<'PY'
import os, socket, sys, secrets

proto = os.environ['PROTO']
ip    = os.environ['BOARD_IP']
port  = int(os.environ['PORT'])
n     = int(os.environ['PAYLOAD_BYTES'])
tmo   = float(os.environ['PROBE_TIMEOUT_S'])
iface = os.environ.get('USB_ETH_IFACE', '')

def _bind_iface(s):
    # The HIL Pi runs Tailscale, whose policy routing (ip rule + a
    # private table) otherwise captures the 192.168.x board subnet --
    # an unbound socket to the board would leave via tailscale0 and
    # fail with EHOSTUNREACH. Pin the socket to the USB-Ethernet
    # interface so probe traffic always uses the direct link.
    if iface:
        so_bindtodevice = getattr(socket, 'SO_BINDTODEVICE', 25)
        s.setsockopt(socket.SOL_SOCKET, so_bindtodevice, iface.encode())

if proto == 'tcp':
    payload = secrets.token_bytes(n)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        _bind_iface(s)
        s.settimeout(tmo)
        s.connect((ip, port))
        s.sendall(payload)
        got = b''
        while len(got) < n:
            chunk = s.recv(n - len(got))
            if not chunk:
                break
            got += chunk
        s.close()
    except Exception as e:
        print(f"FAIL tcp: {e}", file=sys.stderr); sys.exit(1)
    if got != payload:
        print(f"FAIL tcp: sent {n} bytes, got {len(got)}; "
              f"first mismatch at {next((i for i,(a,b) in enumerate(zip(payload,got)) if a!=b), 'n/a')}",
              file=sys.stderr)
        sys.exit(1)
    print(f"OK tcp: {n} bytes round-tripped byte-exact")
    sys.exit(0)

if proto == 'udp':
    payload = secrets.token_bytes(min(n, 1024))  # keep below typical MTU
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    _bind_iface(s)
    s.settimeout(tmo)
    try:
        # Some boards drop the very first UDP datagram pending ARP. Retry up to 3x.
        got = None
        for _ in range(3):
            s.sendto(payload, (ip, port))
            try:
                data, _ = s.recvfrom(2048)
                got = data
                break
            except socket.timeout:
                continue
        if got is None:
            print(f"FAIL udp: no reply from {ip}:{port} within {tmo}s",
                  file=sys.stderr)
            sys.exit(1)
    finally:
        s.close()
    if got != payload:
        print(f"FAIL udp: sent {len(payload)}, got {len(got)} (mismatch)",
              file=sys.stderr)
        sys.exit(1)
    print(f"OK udp: {len(payload)} bytes round-tripped byte-exact")
    sys.exit(0)

if proto == 'http':
    req = f"GET / HTTP/1.1\r\nHost: {ip}\r\nConnection: close\r\n\r\n".encode()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        _bind_iface(s)
        s.settimeout(tmo)
        s.connect((ip, port))
        s.sendall(req)
        buf = b''
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
            if len(buf) > 65536:
                break
        s.close()
    except Exception as e:
        print(f"FAIL http: {e}", file=sys.stderr); sys.exit(1)
    if b"200 OK" not in buf:
        print(f"FAIL http: no '200 OK' in response: {buf[:200]!r}", file=sys.stderr)
        sys.exit(1)
    if b"Hello from RA8D2" not in buf:
        print(f"FAIL http: marker 'Hello from RA8D2' not in body: {buf[:200]!r}", file=sys.stderr)
        sys.exit(1)
    print(f"OK http: 200 OK + marker present ({len(buf)} bytes received)")
    sys.exit(0)

print(f"FAIL: unknown proto {proto!r}", file=sys.stderr)
sys.exit(1)
PY
)" || PROBE_RC=$?
PROBE_RC="${PROBE_RC:-0}"

echo "$RESULT"

if ((PROBE_RC == 0)); then
  echo -e "${GREEN}[HIL PASS]${NC} ${APP_NAME}"
  exit 0
else
  echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: probe failed"
  exit 1
fi

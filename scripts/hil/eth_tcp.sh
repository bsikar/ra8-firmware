#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
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
# All wire-side probing is done on the Pi. The board is wired to the physical
# built-in Ethernet interface declared by the fleet model.
#
# The root-owned privileged policy verifies that exact interface's permanent
# MAC, canonical sysfs device, PHC, and non-uplink status. USB-Ethernet
# adapters are intentionally rejected because they do not provide the required
# hardware clock. The helper temporarily assigns 192.168.1.1/24 for the test
# and restores the prior state on exit.
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

if [[ "$-" == *p* ]]; then
  unset -v BASH_ENV ENV
  declare -a ra8_startup_env_unset=()
  _ra8_startup_refuse() {
    printf 'error: privileged startup %s\n' "$1" >&2
    exit 1
  }
  ra8_startup_env_done_count=0
  while IFS= read -r -d '' ra8_startup_env_row; do
    ra8_startup_env_name="${ra8_startup_env_row%%=*}"
    case "$ra8_startup_env_name" in
      RA8_STARTUP_ENV_DONE)
        ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))
        ;;
      BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;
    esac
  done < <(
    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&
      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\0'
  )
  ((ra8_startup_env_done_count == 1)) && [[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || _ra8_startup_refuse 'environment enumeration was incomplete'
  if ((${#ra8_startup_env_unset[@]})); then
    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || _ra8_startup_refuse 'scrub did not converge'
    ra8_startup_reentry="$0"
    [[ "$ra8_startup_reentry" == */* ]] || _ra8_startup_refuse 'requires a script path'
    if [[ "$ra8_startup_reentry" != /* ]]; then
      ra8_startup_reentry="$PWD/$ra8_startup_reentry"
    fi
    ra8_startup_check="$ra8_startup_reentry"
    while [[ "$ra8_startup_check" != "/" ]]; do
      [[ ! -L "$ra8_startup_check" ]] || _ra8_startup_refuse 'refuses a symlinked path'
      ra8_startup_parent="${ra8_startup_check%/*}"
      [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"
      [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||
        _ra8_startup_refuse 'cannot validate its script path'
      ra8_startup_check="$ra8_startup_parent"
    done
    [[ -f "$ra8_startup_reentry" ]] || _ra8_startup_refuse 'refuses a non-regular path'
    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \
      -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \
      /bin/bash -p -- "$ra8_startup_reentry" "$@"; then
      _ra8_startup_refuse 'could not enter sanitized process'
    fi
  fi
  unset -v ra8_startup_check ra8_startup_env_done_count
  unset -v ra8_startup_env_name ra8_startup_env_row
  unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry
  unset -v RA8_STARTUP_ENV_DONE
  unset -v RA8_STARTUP_ENV_SCRUBBED
  unset -f _ra8_startup_refuse

  set -euo pipefail

  # This script re-execs ITSELF on the Pi, and it gets there down a pipe
  # (`ssh ... bash -s < "$0"`). A script read from stdin has no BASH_SOURCE and no
  # sibling `lib/` to source, so the remote half cannot repeat the local half's
  # setup: under `set -u` it aborted on `${BASH_SOURCE[0]}` before printing a
  # single line, which took the tree's only wire-side gate off the air whenever it
  # was driven from a dev machine. The remote half therefore takes its rig values
  # from the environment (exported on the ssh command line by the local half) and
  # its console resolver from the front of the same pipe -- and it must NOT take
  # the bench a second time, because the local half is already holding it and
  # would be denied by its own lock.
  if [[ -n "${RA8_HIL_ETH_REMOTE:-}" ]]; then
    RA8_TTY_RESOLVER_SRC=""
    RA8_HIL_PRIVILEGED_HELPER="/usr/local/libexec/ra8-hil-privileged"
  else
    # Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
    _hil_dir="$(dirname "${BASH_SOURCE[0]}")"
    _hil_dir="$(cd "$_hil_dir" && pwd)"
    # shellcheck source=scripts/hil/lib/rig_env.sh
    source "$_hil_dir/lib/rig_env.sh"
    rig_require PI_HOST JLINK_SN
    # shellcheck source=scripts/hil/lib/privileged_helper.sh
    source "$_hil_dir/lib/privileged_helper.sh"

    # ---- bench mutual exclusion ------------------------------------------------
    # One actor at a time on the physical bench. The hold lives exactly as long as
    # this script does -- it is a live process on a kernel flock, not a lease -- so
    # nothing here can leave the bench stale. See scripts/hil/bench.sh.
    # shellcheck source=scripts/hil/lib/bench_lock.sh
    source "$_hil_dir/lib/bench_lock.sh"
    ra8_bench_require "hil ethernet tcp probe $*" 30m || exit $?
  fi
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
  # Resolved by identity on the Pi before the console is read; see below.
  UART=""
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

  valid_uint_range() {
    local value="$1"
    local minimum="$2"
    local maximum="$3"
    local label="$4"
    if [[ ! "$value" =~ ^[1-9][0-9]*$ || ${#value} -gt 10 ]] ||
      ((10#$value < minimum || 10#$value > maximum)); then
      echo "$label must be an integer in ${minimum}..${maximum}" >&2
      exit 2
    fi
  }

  if [[ ! "$BOARD_IP" =~ ^192\.168\.1\.([0-9]{1,3})$ ]]; then
    echo "--board-ip must be a host in the isolated 192.168.1.0/24 bench subnet" >&2
    exit 2
  fi
  board_octet="${BASH_REMATCH[1]}"
  if [[ ("$board_octet" == 0*) || ${#board_octet} -gt 3 ]] ||
    ((10#$board_octet < 2 || 10#$board_octet > 254)); then
    echo "--board-ip must be canonical and must not collide with 192.168.1.1" >&2
    exit 2
  fi
  valid_uint_range "$PORT" 1 65535 "--port"
  valid_uint_range "$PAYLOAD_BYTES" 1 1048576 "--payload-bytes"
  valid_uint_range "$BOOT_TIMEOUT_S" 1 300 "--boot-timeout"
  valid_uint_range "$PROBE_TIMEOUT_S" 1 120 "--probe-timeout"
  valid_uint_range "$BAUD" 1200 4000000 "--baud"

  APP_NAME="$(basename "${HEX%.hex}")"

  # The workstation side validates the full image before it is staged. When this
  # script is invoked directly on the Pi, the ordinary branch also owns _hil_dir
  # and performs the same check. The piped remote recursion inherits an image that
  # has already passed and has no sibling library tree to source.
  if [[ -z "${RA8_HIL_ETH_REMOTE:-}" ]]; then
    # shellcheck source=scripts/hil/lib/preflash_guard.sh
    source "$_hil_dir/lib/preflash_guard.sh"
    ra8_preflash_guard "$HEX" || exit $?
  fi

  LOCAL_PI=0
  if [[ -n "${RA8_HIL_ETH_REMOTE:-}" ]]; then
    # We ARE the Pi-side half; rig_is_local_pi is not available here.
    LOCAL_PI=1
  elif rig_is_local_pi; then
    LOCAL_PI=1
  fi
  if [[ -n "${RA8_HIL_ETH_REMOTE:-}" ]]; then
    expected_identity="${RA8_HIL_PRIVILEGED_EXPECTED:-}"
    [[ "$expected_identity" =~ ^[0-9a-f]{64}:[0-9a-f]{64}$ ]] || {
      echo "[HIL] remote privileged helper identity was not supplied" >&2
      exit 2
    }
    actual_identity="$("$RA8_HIL_PRIVILEGED_HELPER" --identity 2>/dev/null)" || {
      echo "[HIL] remote privileged helper identity query failed" >&2
      exit 2
    }
    [[ "$actual_identity" == "$expected_identity" ]] || {
      echo "[HIL] remote privileged helper differs from the driving checkout" >&2
      exit 2
    }
  elif ((LOCAL_PI)); then
    ra8_hil_privileged_verify_local || exit $?
  fi

  # Off-Pi: scp the hex over, re-invoke ourselves on the Pi.
  if ((LOCAL_PI == 0)); then
    ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
      {
        echo -e "${RED}[HIL]${NC} cannot reach ${PI_HOST}"
        exit 2
      }
    ra8_hil_privileged_verify_remote "$PI_HOST" || exit $?
    expected_identity="$(ra8_hil_privileged_expected_identity)" || exit $?
    REMOTE_HEX="/tmp/$(basename "$HEX")"
    scp -q "$HEX" "${PI_HOST}:${REMOTE_HEX}"
    if [[ -f "${HEX%.hex}.elf" ]]; then
      scp -q "${HEX%.hex}.elf" "${PI_HOST}:${REMOTE_HEX%.hex}.elf"
    fi
    # Forward --uart only when one was named: an empty value would be quoted
    # through as a real (empty) device, and the Pi resolves its own console by
    # identity when left to it.
    remote_args=(
      env
      "RA8_HIL_ETH_REMOTE=1"
      "RA8_HIL_PRIVILEGED_EXPECTED=$expected_identity"
      "JLINK_SN=$JLINK_SN"
      "JLINK_DEVICE=$JLINK_DEVICE"
      /bin/bash -p -s --
      --hex "$REMOTE_HEX"
      --board-ip "$BOARD_IP"
      --port "$PORT"
      --proto "$PROTO"
      --payload-bytes "$PAYLOAD_BYTES"
      --boot-timeout "$BOOT_TIMEOUT_S"
      --probe-timeout "$PROBE_TIMEOUT_S"
    )
    [[ -n "$UART" ]] && remote_args+=(--uart "$UART")
    remote_args+=(--baud "$BAUD")
    printf -v remote_command '%q ' "${remote_args[@]}"
    # The remote half needs the console resolver but has no checkout to source it
    # from, so it rides in at the FRONT of the same pipe that carries this script.
    # Everything else it needs is a scalar and goes on the ssh command line.
    # shellcheck disable=SC2029  # every value is local rig/app config passed as args to the piped script.
    cat <(printf '%s\n' "$RA8_TTY_RESOLVER_SRC") "$0" |
      ssh "$PI_HOST" "$remote_command"
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

  # ---- 2. Pre-arm the board-facing Ethernet interface --------------------------
  # The root-owned installed policy is the only interface authority. The helper
  # verifies its permanent MAC, canonical sysfs device, PHC and all-table uplink
  # status before mutation; the shell receives only the already-authenticated
  # name for unprivileged diagnostics.
  USB_ETH_IFACE="$("$RA8_HIL_PRIVILEGED_HELPER" --policy-interface 2>/dev/null)" || {
    echo -e "${RED}[HIL]${NC} privileged helper policy query failed" >&2
    exit 2
  }
  [[ "$USB_ETH_IFACE" =~ ^[A-Za-z][A-Za-z0-9_-]{0,14}$ ]] || {
    echo -e "${RED}[HIL]${NC} privileged helper returned an invalid interface" >&2
    exit 2
  }
  echo -e "${YELLOW}[HIL]${NC} board-facing iface = ${USB_ETH_IFACE}"

  # shellcheck disable=SC2329  # invoked from cleanup(), itself a trap handler.
  restore_iface() {
    # The root helper accepts no cleanup arguments: it removes only mutations
    # authenticated by its root-owned state record.
    sudo -n -- "$RA8_HIL_PRIVILEGED_HELPER" net-cleanup 2>/dev/null || true
  }
  # shellcheck disable=SC2329  # invoked by `trap cleanup EXIT` below.
  cleanup() {
    rm -f "$STRIPPED_HEX" "/tmp/hil_eth_jlink_${APP_NAME}.cmd"
    # Guarded: UART is still empty until the console is resolved further down,
    # and a pkill pattern of a bare "cat " would match unrelated processes.
    # The `|| true` is load-bearing: pkill exits 1 when it matches nothing, and
    # under `set -e` a failing command inside an EXIT trap ABORTS the trap -- so
    # on every run where the console reader had already gone, restore_iface below
    # was never reached and the temporary address stayed on the interface.
    if [ -n "$UART" ]; then
      pkill -f "cat ${UART}" 2>/dev/null || true
    fi
    restore_iface
  }
  trap cleanup EXIT

  # Assign 192.168.1.1/24 to the iface (idempotent).
  #
  # `noprefixroute` + an explicit host route to the board is load-bearing, not
  # tidiness. The bench Pi's own LAN is 192.168.1.0/24 as well, so the connected
  # route the kernel would otherwise install for this address takes the WHOLE of
  # that prefix and points it at the board-facing NIC, where nothing but the board
  # answers. Everything else on the LAN then disappears from the Pi -- including
  # the Tapo plug that powers the board, i.e. the one lever that recovers a wedged
  # bench. `restore_iface` runs from an EXIT trap, so a run that is killed (or
  # whose ssh dies) leaves that hijack in place indefinitely; this way there is no
  # hijack to leave behind. A host route reaches the board just as well.
  if ! sudo -n -- "$RA8_HIL_PRIVILEGED_HELPER" \
    net-prepare "$BOARD_IP" 2>/dev/null; then
    echo -e "${RED}[HIL]${NC} privileged helper refused the isolated Ethernet setup" >&2
    exit 1
  fi
  # Drop any neighbour entry left over from a previous run. The board is held in
  # reset for the duration of the flash below, so the previous run's exit -- or
  # any probe that ran while the board was down -- leaves the board IP cached in
  # the FAILED state. Linux then answers from that cache instead of re-ARPing,
  # and the freshly-booted board looks unreachable for as long as the entry
  # lives. Symptom: back-to-back runs alternate PASS / "no ICMP/ARP reply" with
  # metronomic regularity, which reads as a flaky board and is not one.
  if ! sudo -n -- "$RA8_HIL_PRIVILEGED_HELPER" net-neigh-flush 2>/dev/null; then
    echo -e "${YELLOW}[HIL]${NC} could not flush the stale neighbour cache on ${USB_ETH_IFACE}" >&2
  fi

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

  # Resolve the board console by device identity unless one was named
  # explicitly (scripts/hil/lib/tty_resolve.sh).
  if [ -z "$UART" ]; then
    UART="$(ra8_tty_resolve console)" || exit 1
  fi
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
    echo "    packet capture unavailable: the HIL account intentionally has no"
    echo "    privileged capture executable; use an operator-owned diagnostic session"
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
    # fail with EHOSTUNREACH. Pin the socket to the fleet-declared
    # board-facing interface so probe traffic always uses the direct link.
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
else
  [[ "$-" == *p* ]]
fi

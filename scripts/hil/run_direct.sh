#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_run_direct.sh -- Flash a firmware .hex and verify expected UART output.
#
# Auto-detects environment:
#  * If running ON the HIL Pi (see rig_is_local_pi), runs JLinkExe and reads
#    the board console directly. The console is resolved by device identity
#    (scripts/hil/lib/tty_resolve.sh), never by ttyACM number.
#  * If running on a developer workstation, SCPs the hex to the Pi, SSHes
#    in, and re-invokes itself there. The shape of the test (expect string,
#    timeout) is preserved unchanged.
#
# Usage (works from either side):
#   scripts/hil/run_direct.sh --hex <path/to/app.hex> \
#                             --expect <string>            \
#                             [--expect-negative <regex>]  \
#                             [--provision-wifi]           \
#                             [--baud 115200]              \
#                             [--timeout 10]               \
#                             [--uart <device>]
#
# --expect-negative <regex>
#     Extended-regex (grep -E) of substrings that, if seen in the UART
#     log, fail the run -- even when the positive --expect matched.
#     Closes the class of weak hil.conf where HIL_EXPECT="fxlx"
#     matches both "fxlx: booting" and "fxlx: lx_nor_flash_format
#     failed". Per-app hil.conf passes this in as HIL_EXPECT_NEGATIVE.
#
# Sanity gates run on the inputs themselves (rejected with exit 2
# before flashing):
#   - --expect must be >= 12 chars unless HIL_EXPECT_SHORT_OK=1 in the
#     environment (small-app escape hatch documented in the README).
#   - --expect must not be a substring of any failure-banner string in
#     the .elf .rodata (where "failure banner" means a string matching
#     /FAIL|panic|NAK|ERROR|failed/ via arm-none-eabi-strings).
#
# Exit codes:
#   0  PASS  -- positive expect matched within timeout AND no negative
#               expect (if set) matched in the captured log
#   1  FAIL  -- timeout elapsed without positive match, OR negative
#               expect matched, OR flash failed
#   2  ERROR -- missing arguments, hardware not reachable, or sanity
#               gates rejected the input

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

  # Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
  #
  # The `:-$0` fallback is load-bearing, not defensive noise. The off-Pi branch
  # below re-invokes THIS FILE on the bench host by piping it into `bash -s`, and
  # a script read from stdin has an EMPTY BASH_SOURCE array -- so under `set -u`
  # the remote copy aborted here, on its first line of work, with
  # "BASH_SOURCE[0]: unbound variable". Every off-Pi `uart_scrape` run went that
  # way, which is every app `just hil::suite` verifies. With the fallback, `$0` is
  # `bash` and this resolves to the working directory the ssh command sets --
  # which is why that command cds into the bench host's checkout.
  _hil_dir="$(dirname "${BASH_SOURCE[0]:-$0}")"
  _hil_dir="$(cd "$_hil_dir" && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require PI_HOST JLINK_SN JLINK_DEVICE

  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[1;33m'
  NC='\033[0m'

  usage() {
    echo "Usage: $0 --hex <file> --expect <string> [--expect-negative <regex>] [--provision-wifi] [--baud 115200] [--timeout 10] [--uart <device>]"
    exit 2
  }

  HEX=""
  EXPECT=""
  EXPECT_NEG="${HIL_EXPECT_NEGATIVE:-}"
  BAUD="115200"
  TIMEOUT_S="10"
  # Empty on purpose: the console is resolved by identity below, and there is no
  # ttyACM number that is right often enough to be a default.
  UART=""
  UART_EXPLICIT=""
  PROVISION_WIFI=0
  PROVISION_STARTUP_GRACE_S=120
  # Internal off-Pi transport flag: relay only the non-secret READY prompt while
  # the outer process owns packet delivery on a separate SSH channel.
  RELAY_PROVISION_READY=0

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --hex)
        (($# >= 2)) || usage
        HEX="$2"
        shift 2
        ;;
      --expect)
        (($# >= 2)) || usage
        EXPECT="$2"
        shift 2
        ;;
      --expect-negative)
        (($# >= 2)) || usage
        EXPECT_NEG="$2"
        shift 2
        ;;
      --provision-wifi)
        PROVISION_WIFI=1
        shift
        ;;
      --relay-provision-ready)
        RELAY_PROVISION_READY=1
        shift
        ;;
      --baud)
        (($# >= 2)) || usage
        BAUD="$2"
        shift 2
        ;;
      --timeout)
        (($# >= 2)) || usage
        TIMEOUT_S="$2"
        shift 2
        ;;
      --uart)
        (($# >= 2)) || usage
        UART="$2"
        UART_EXPLICIT=1
        shift 2
        ;;
      *)
        echo "Unknown arg: $1"
        usage
        ;;
    esac
  done

  [[ -z "$HEX" || -z "$EXPECT" ]] && usage
  [[ -f "$HEX" ]] || {
    echo -e "${RED}[HIL]${NC} hex not found: $HEX"
    exit 2
  }
  if [[ ! "$BAUD" =~ ^[0-9]+$ ]] || ((10#$BAUD < 1200 || 10#$BAUD > 4000000)); then
    echo -e "${RED}[HIL]${NC} --baud must be a decimal value from 1200 to 4000000" >&2
    exit 2
  fi
  if [[ ! "$TIMEOUT_S" =~ ^[0-9]+$ ]] || ((10#$TIMEOUT_S < 1 || 10#$TIMEOUT_S > 3600)); then
    echo -e "${RED}[HIL]${NC} --timeout must be a decimal value from 1 to 3600" >&2
    exit 2
  fi
  BAUD=$((10#$BAUD))
  TIMEOUT_S=$((10#$TIMEOUT_S))
  PROVISION_PROVIDER_TIMEOUT_S=$TIMEOUT_S
  if ((PROVISION_PROVIDER_TIMEOUT_S > 600)); then
    PROVISION_PROVIDER_TIMEOUT_S=600
  fi
  if [[ -n "$UART_EXPLICIT" && "$UART" != /dev/* ]]; then
    echo -e "${RED}[HIL]${NC} --uart must name a device below /dev" >&2
    exit 2
  fi

  # Sanity gate 1: minimum positive-expect length.
  # "fxlx" (4 chars) is the canonical failure case -- it appears in both
  # "fxlx: booting..." (success) and "fxlx: lx_nor_flash_format failed"
  # (failure). 12 chars effectively requires a colon-banner-with-words
  # pattern that can't accidentally overlap with a failure path.
  MIN_EXPECT_LEN=12
  if [[ "${HIL_EXPECT_SHORT_OK:-0}" != "1" && ${#EXPECT} -lt $MIN_EXPECT_LEN ]]; then
    echo -e "${RED}[HIL]${NC} --expect='${EXPECT}' is too short (${#EXPECT} < ${MIN_EXPECT_LEN} chars)."
    echo "    Tighten the banner so it can't accidentally match a failure path."
    echo "    Override (not recommended): HIL_EXPECT_SHORT_OK=1"
    exit 2
  fi

  # Sanity gate 2: overlap with .elf failure banners.
  # Pull every string >= len(EXPECT) from the matching .elf via
  # arm-none-eabi-strings; if any string that contains EXPECT also
  # contains a failure word, the EXPECT pattern can match the
  # failure banner -- reject before flashing.
  ELF_FOR_STRINGS="${HEX%.hex}.elf"
  if [[ -f "$ELF_FOR_STRINGS" ]] && command -v arm-none-eabi-strings >/dev/null 2>&1; then
    OVERLAP_HITS=$(arm-none-eabi-strings -n "${MIN_EXPECT_LEN}" "$ELF_FOR_STRINGS" 2>/dev/null |
      grep -F -- "$EXPECT" |
      grep -iE '\b(FAIL|FAILED|panic|NAK|ERROR|failed|HardFault|MemFault|BusFault|UsageFault|abort)\b' |
      head -3 || true)
    if [[ -n "$OVERLAP_HITS" ]]; then
      echo -e "${RED}[HIL]${NC} --expect='${EXPECT}' overlaps failure banners in $(basename "$ELF_FOR_STRINGS"):"
      while IFS= read -r line; do echo "    + $line"; done <<<"$OVERLAP_HITS"
      echo "    Pick a HIL_EXPECT that is unique to the success path."
      echo "    Override (not recommended): HIL_EXPECT_OVERLAP_OK=1"
      if [[ "${HIL_EXPECT_OVERLAP_OK:-0}" != "1" ]]; then
        exit 2
      fi
      echo -e "${YELLOW}[HIL]${NC} HIL_EXPECT_OVERLAP_OK=1 set -- continuing despite overlap."
    fi
  fi

  APP_NAME="$(basename "${HEX%.hex}")"
  [[ "$APP_NAME" =~ ^[A-Za-z0-9_.-]+$ ]] || {
    echo -e "${RED}[HIL]${NC} image basename contains unsupported characters: ${APP_NAME}" >&2
    exit 2
  }
  # Per-invocation, not fixed: /tmp/hil_jlink_<app>.log and /tmp/hil_uart_<app>.log
  # were shared by every actor running the same app, so two runs interleaved into
  # one log and each read the other's bytes.
  LOG_FILE="/tmp/hil_jlink_${APP_NAME}.$$.log"

  # Detect whether we're already on the Pi (matches the pattern used by
  # hil_usb_test.sh: hostname OR aarch64 with a CDC device attached).
  LOCAL_PI=0
  if rig_is_local_pi; then
    LOCAL_PI=1
  fi

  # ---- bench mutual exclusion --------------------------------------------------
  # Taken on the WORKSTATION side (before the re-invocation below) and inherited
  # through the ssh by RA8_BENCH_LOCK_ID, so one run holds the bench end to end
  # rather than dropping it between the flash and the scrape. See
  # scripts/hil/bench.sh.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require "hil run ${APP_NAME}" "$((TIMEOUT_S + 300))s" || exit $?

  # Inspect the full image before any OFS stripping or J-Link programming. This
  # also protects direct invocations outside all.sh.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$_hil_dir/lib/preflash_guard.sh"
  ra8_preflash_guard "$HEX" || exit $?

  # Off-Pi: scp the hex over, re-invoke ourselves on the Pi with --hex
  # pointing at the remote copy. This way every Pi-local step (JLinkExe,
  # console reads) runs natively, and the developer just sees the
  # pass/fail on stdout.
  if ((LOCAL_PI == 0)); then
    ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
      {
        echo -e "${RED}[HIL]${NC} cannot reach ${PI_HOST}"
        exit 2
      }
    REMOTE_DIR="/tmp/ra8_hil_${APP_NAME}_$$_${RANDOM}"
    REMOTE_LOG=""
    REMOTE_PID=""
    REMOTE_TEE_PID=""
    REMOTE_STREAM_DIR=""
    REMOTE_FIFO=""
    printf -v REMOTE_DIR_Q '%q' "$REMOTE_DIR"
    # shellcheck disable=SC2329  # registered with the shared EXIT dispatcher below.
    cleanup_remote_stage() {
      if [[ -n "$REMOTE_PID" ]] && kill -0 "$REMOTE_PID" 2>/dev/null; then
        kill "$REMOTE_PID" 2>/dev/null || true
        wait "$REMOTE_PID" 2>/dev/null || true
      fi
      if [[ -n "$REMOTE_TEE_PID" ]] && kill -0 "$REMOTE_TEE_PID" 2>/dev/null; then
        kill "$REMOTE_TEE_PID" 2>/dev/null || true
        wait "$REMOTE_TEE_PID" 2>/dev/null || true
      fi
      [[ -z "$REMOTE_FIFO" ]] || rm -f "$REMOTE_FIFO"
      [[ -z "$REMOTE_STREAM_DIR" ]] || rmdir "$REMOTE_STREAM_DIR" 2>/dev/null || true
      [[ -z "$REMOTE_LOG" ]] || rm -f "$REMOTE_LOG"
      ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" \
        "rm -rf -- ${REMOTE_DIR_Q}" >/dev/null 2>&1 || true
      return 0
    }
    _ra8_bench_add_exit_trap cleanup_remote_stage
    # shellcheck disable=SC2029  # The unique staging path is deliberately composed by this client.
    ssh "$PI_HOST" "mkdir -p -- ${REMOTE_DIR_Q}"
    REMOTE_HEX="${REMOTE_DIR}/firmware.hex"
    scp -q "$HEX" "${PI_HOST}:${REMOTE_HEX}"
    # If an ELF sibling exists, copy it too -- the OFS-strip path uses it.
    if [[ -f "${HEX%.hex}.elf" ]]; then
      scp -q "${HEX%.hex}.elf" "${PI_HOST}:${REMOTE_HEX%.hex}.elf"
    fi
    # Re-invoke ourselves on the Pi. Every value crossing the remote shell is
    # individually quoted, including paths and inherited lock identifiers.
    declare -a REMOTE_ARGV=(--hex "$REMOTE_HEX" --expect "$EXPECT")
    if [[ -n "$EXPECT_NEG" ]]; then
      REMOTE_ARGV+=(--expect-negative "$EXPECT_NEG")
    fi
    REMOTE_ARGV+=(--baud "$BAUD" --timeout "$TIMEOUT_S")
    if ((PROVISION_WIFI == 1)); then
      REMOTE_ARGV+=(--relay-provision-ready)
    fi
    # Only forward --uart when the caller actually named one. Forwarding an
    # empty value would pin the remote side to nothing; without it the Pi
    # resolves its own console by identity, which is what it should do anyway.
    if [[ -n "$UART_EXPLICIT" ]]; then
      REMOTE_ARGV+=(--uart "$UART")
    fi
    # Propagate the sanity-gate escape-hatch flags so the remote side
    # doesn't re-reject inputs the local side already vetted.
    REMOTE_ENV=""
    [[ "${HIL_EXPECT_SHORT_OK:-0}" == "1" ]] && REMOTE_ENV+="HIL_EXPECT_SHORT_OK=1 "
    [[ "${HIL_EXPECT_OVERLAP_OK:-0}" == "1" ]] && REMOTE_ENV+="HIL_EXPECT_OVERLAP_OK=1 "
    # Carry the bench hold across the ssh. Without this the Pi-side copy would
    # see no lock in its environment and try to take a SECOND one -- against the
    # hold this side is already keeping alive -- and deadlock against itself.
    if [[ -n "${RA8_BENCH_LOCK_ID:-}" ]]; then
      printf -v REMOTE_LOCK_Q '%q' "$RA8_BENCH_LOCK_ID"
      REMOTE_ENV+="RA8_BENCH_LOCK_ID=${REMOTE_LOCK_Q} "
    fi
    # The piped copy resolves its own `lib/` relative to the working directory
    # (see the BASH_SOURCE note at the top of this file), so the ssh command has
    # to put it somewhere those libraries exist: the bench host's own checkout,
    # named by PI_REPO. Unlike run.sh -- which pastes the tty resolver into a
    # self-contained heredoc precisely so the bench host needs no checkout -- this
    # script sends itself whole and therefore needs one. `cd || exit` fails the
    # run loudly rather than letting it proceed from the wrong directory.
    printf -v REMOTE_ARGS_Q '%q ' "${REMOTE_ARGV[@]}"
    printf -v REMOTE_CWD_Q '%q' "${PI_REPO}/scripts/hil"
    REMOTE_COMMAND="cd ${REMOTE_CWD_Q} || exit 2; ${REMOTE_ENV}/bin/bash -p -s -- ${REMOTE_ARGS_Q}"
    REMOTE_RC=0
    if ((PROVISION_WIFI == 1)); then
      REMOTE_LOG="$(mktemp "${TMPDIR:-/tmp}/ra8-wifi-hil.XXXXXX")"
      REMOTE_STREAM_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ra8-wifi-stream.XXXXXX")"
      REMOTE_FIFO="$REMOTE_STREAM_DIR/stdout"
      mkfifo "$REMOTE_FIFO"
      # The recursive runner still receives this script on stdin. A second SSH
      # channel carries only the runtime packet on stdin after the firmware's
      # fresh-boot prompt, so no credential enters argv, a file, or build output.
      tee "$REMOTE_LOG" <"$REMOTE_FIFO" &
      REMOTE_TEE_PID=$!
      # shellcheck disable=SC2029  # all interpolated values in REMOTE_COMMAND are shell-quoted above.
      ssh "$PI_HOST" "$REMOTE_COMMAND" <"$0" >"$REMOTE_FIFO" 2>&1 &
      REMOTE_PID=$!
      PROVISION_READY=0
      PROVISION_DEADLINE=$((SECONDS + PROVISION_STARTUP_GRACE_S + TIMEOUT_S))
      while ((SECONDS < PROVISION_DEADLINE)); do
        if grep -qF "ra8_net_provision: READY v1" "$REMOTE_LOG"; then
          PROVISION_READY=1
          break
        fi
        if ! kill -0 "$REMOTE_PID" 2>/dev/null; then
          break
        fi
        sleep 0.1
      done
      if ((PROVISION_READY == 1)); then
        printf -v _pi_repo_q '%q' "${PI_REPO}/scripts/hil"
        if ! python3 "$_hil_dir/../secrets/wifi_provision.py" \
          --timeout "$PROVISION_PROVIDER_TIMEOUT_S" emit |
          ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" "cd ${_pi_repo_q} || exit 2
          source lib/rig_env.sh
          provision_uart=\$(ra8_tty_resolve console) || exit 2
          stty -F \"\$provision_uart\" ${BAUD} raw -echo
          cat >\"\$provision_uart\""; then
          echo -e "${RED}[HIL]${NC} runtime Wi-Fi provisioning failed" >&2
          REMOTE_RC=1
        fi
      else
        echo -e "${RED}[HIL]${NC} runtime Wi-Fi provisioning prompt was not observed" >&2
        REMOTE_RC=1
        kill "$REMOTE_PID" 2>/dev/null || true
      fi
      REMOTE_WAIT_RC=0
      wait "$REMOTE_PID" || REMOTE_WAIT_RC=$?
      REMOTE_TEE_RC=0
      wait "$REMOTE_TEE_PID" || REMOTE_TEE_RC=$?
      if ((REMOTE_WAIT_RC != 0 && REMOTE_RC == 0)); then
        REMOTE_RC=$REMOTE_WAIT_RC
      fi
      if ((REMOTE_TEE_RC != 0 && REMOTE_RC == 0)); then
        REMOTE_RC=$REMOTE_TEE_RC
      fi
      rm -f "$REMOTE_LOG"
      rm -f "$REMOTE_FIFO"
      rmdir "$REMOTE_STREAM_DIR"
      REMOTE_LOG=""
      REMOTE_PID=""
      REMOTE_TEE_PID=""
      REMOTE_STREAM_DIR=""
      REMOTE_FIFO=""
    else
      # shellcheck disable=SC2029  # all interpolated values in REMOTE_COMMAND are shell-quoted above.
      ssh "$PI_HOST" "$REMOTE_COMMAND" <"$0" ||
        REMOTE_RC=$?
    fi
    exit "$REMOTE_RC"
  fi

  echo -e "${YELLOW}[HIL]${NC} app=${APP_NAME}  expect='${EXPECT}'  timeout=${TIMEOUT_S}s"

  # ---- 1. Strip OFS sections ---------------------------------------------------
  # OFS sections at 0x0300A100+ cause J-Link RAMCode to timeout during Prepare()
  # when TrustZone option bytes are involved.  Strip them so J-Link only programs
  # the MRAM bank at 0x02000000.
  ELF="${HEX%.hex}.elf"
  STRIPPED_HEX="/tmp/hil_${APP_NAME}_mram.$$.hex"
  OFS_ARGS=('--remove-section=.option_setting*')
  if [[ -f "$ELF" ]]; then
    arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null ||
      cp "$HEX" "$STRIPPED_HEX"
  else
    declare -a MRAM_SECTION_ARGS=()
    while IFS= read -r section_arg; do
      MRAM_SECTION_ARGS+=("$section_arg")
    done < <(
      arm-none-eabi-objdump -h "$HEX" 2>/dev/null |
        awk '$4 ~ /^020[0-9A-Fa-f]{5}$/ { print "--only-section=" $2 }'
    )
    if ((${#MRAM_SECTION_ARGS[@]} == 0)); then
      echo -e "${RED}[HIL]${NC} no MRAM sections found in ${HEX}" >&2
      exit 2
    fi
    arm-none-eabi-objcopy -I ihex "${MRAM_SECTION_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_HEX"
  fi

  # ---- 2. Flash via J-Link (loadfile with OFS-stripped hex) -------------------
  # Direct w4 writes don't commit to MRAM cells without the MRMS flush sequence
  # (MRCPC1 gate + MRCFLR flush per HUM Ch 59).  Implementing that in J-Link
  # Commander is impractical, so we use loadfile (which uses RAMCode internally).
  # OFS stripping above prevents the RAMCode Prepare() timeout that occurs when
  # .option_setting* sections at 0x0300A100 are included.
  #
  # Pre-flash LPSCR clear (HUM Ch 11.2.18 / 11.2.20):
  # Some apps (e.g. power_profiler) write LPSCR.LPMD = 0x4 (software standby)
  # before WFI.  SYSRESETREQ via the debugger does NOT reset the SYSC LPM block
  # (separate reset domain), so LPSCR survives reset.  When J-Link's RAMCode
  # helper later executes any WFI, it gets trapped into software standby with
  # no wake source -- "RAMCode did not respond" timeout cascades.  Clearing
  # LPSCR via DAP (PRCR-unlock + write 0 + relock) makes RAMCode's WFI a plain
  # Sleep that any interrupt can wake.
  TMP_SCRIPT="$(mktemp)"
  # Clean up only what THIS run started. The old form was
  # `pkill -f "cat $UART"`, which killed every console reader on the box --
  # including a colleague's `cat` on the same tty, mid-session, with no warning.
  # READER_PID is the one reader we launched; killing it by pid cannot reach
  # anybody else's. (It is still empty here: the reader starts after the flash.)
  # The trailing `true` was meant to make this trap always succeed, and could
  # not: `set -e` is still in force INSIDE an EXIT trap, so the `&&` list ahead
  # of it aborted the trap the moment `kill` failed -- and `kill` fails on the
  # normal path, because the reader has already been killed and reaped by then.
  # A trap that aborts sets the script's exit status to the failure, which
  # overrode the `exit 0` at the bottom of this file: every PASS was reported to
  # the caller as rc=1, so `just hil::suite` scored zero passes while printing
  # "[HIL PASS]" for each app. Guarding the kill is what actually makes the trap
  # unconditional.
  # shellcheck disable=SC2329  # registered with the shared EXIT dispatcher below.
  cleanup_local_stage() {
    rm -f "$TMP_SCRIPT" "$STRIPPED_HEX"
    if [[ -n "${READY_RELAY_PID:-}" ]]; then
      kill "$READY_RELAY_PID" 2>/dev/null || true
      wait "$READY_RELAY_PID" 2>/dev/null || true
    fi
    if [[ -n "${READER_PID:-}" ]]; then
      kill "$READER_PID" 2>/dev/null || true
      wait "$READER_PID" 2>/dev/null || true
    fi
    return 0
  }
  _ra8_bench_add_exit_trap cleanup_local_stage
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

  echo -e "${YELLOW}[HIL]${NC} flashing ${HEX}..."

  # Start UART reader in the background BEFORE flashing.  The firmware's boot
  # banner prints within milliseconds of "g" (go) -- if we open the console
  # only after JLinkExe returns, one-shot boot banners are missed because the
  # bytes arrive before any reader is attached.  Configure the tty first, then
  # launch a background tail that streams to a log file we can grep afterward.
  #
  # Resolve the console by device identity, with a retry window that covers the
  # J-Link briefly disappearing between back-to-back flash cycles. There is no
  # ttyACM number to default to: the chip's own USBHS CDC (1209:xxxx) and the
  # ESP32-C6's UART bridge enumerate in that namespace too, and which number
  # each takes depends on plug order. See scripts/hil/lib/tty_resolve.sh.
  if [ -z "$UART_EXPLICIT" ]; then
    UART="$(ra8_tty_resolve console)" || exit 1
  fi
  echo -e "${YELLOW}[HIL]${NC} board console: ${UART}"

  # Two readers on one console each get half the bytes, which silently breaks the
  # next test's pattern match -- so a second reader really is a problem. But the
  # fix used to be `pkill -f "cat $UART"`, which killed EVERY reader on the box:
  # a colleague tailing the console to watch their own board lost it, without
  # warning, on every flash anyone did. That is the bench lock's job now. Holding
  # the bench is what keeps a second reader away; if one is here anyway, say so
  # loudly rather than shooting it, because it belongs to somebody.
  if command -v fuser >/dev/null 2>&1; then
    # `|| true` is not decoration: fuser exits 1 when NOBODY has the file open,
    # which is the healthy case and the overwhelmingly common one. Under
    # `set -e` (plus `pipefail`, which makes the pipeline inherit fuser's
    # status) the assignment therefore aborted the run right here, after the
    # "board console:" line and before a single byte was flashed -- so every
    # uart_scrape verification failed with no message at all.
    OTHER_READERS="$(fuser "${UART}" 2>/dev/null | tr -s ' ' | sed 's/^ *//' || true)"
    if [[ -n "$OTHER_READERS" ]]; then
      echo -e "${YELLOW}[HIL]${NC} WARNING: ${UART} is already open by pid(s): ${OTHER_READERS}" >&2
      echo -e "${YELLOW}[HIL]${NC} Two readers split the byte stream. Whoever that is, it is not us --" >&2
      echo -e "${YELLOW}[HIL]${NC} check 'just hil::status'. Not killing it." >&2
    fi
  fi
  stty -F "${UART}" "${BAUD}" raw -echo
  UART_LOG="/tmp/hil_uart_${APP_NAME}.$$.log"
  : >"${UART_LOG}"
  # Drain any stale bytes from the previous test that are still queued in
  # the kernel-side tty receive buffer (`stty` doesn't tcflush). Without
  # this, the new firmware's output is preceded by the previous app's
  # output and the head -20 display truncates the new bytes off-screen.
  # A short non-blocking read + discard is enough; stale buffers are
  # typically a few hundred bytes.
  dd if="${UART}" iflag=nonblock of=/dev/null count=4 bs=1024 2>/dev/null || true
  # Use setsid so the cat does not share our session/process group -- this
  # makes the cleanup pkill at end-of-script reliable regardless of exit path.
  # stdbuf -o0 disables stdout buffering so every byte received from the tty
  # is written to the log file immediately.  Without it, one-shot boot
  # banners (e.g. "ulpt: wake\r\n" = 12 bytes) sit in cat's 4KB output buffer
  # and grep races find an empty log.
  setsid stdbuf -o0 cat "${UART}" >"${UART_LOG}" 2>/dev/null &
  READER_PID=$!
  # Make sure the reader actually opened the tty before we proceed.
  sleep 0.2
  READY_RELAY_PID=""

  # Single attempt: each loadfile op accumulates state in the MRAM controller
  # (~13-op limit before PORST is required).  Retries make the accumulation
  # worse without recovering from it, so we fail fast and let the suite move
  # on; the user can power-cycle and rerun any failed apps.
  JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$TMP_SCRIPT" \
    >"${LOG_FILE}" 2>&1

  if grep -qE "\*\*\*\*\*\* Error|Cannot connect to the probe|could not be halted|RAMCode did not respond|Writing target memory failed" "${LOG_FILE}"; then
    kill "${READER_PID}" 2>/dev/null
    echo -e "${RED}[HIL]${NC} J-Link error -- log tail:" >&2
    tail -20 "${LOG_FILE}" >&2
    exit 1
  fi
  if ! grep -qE "Programming flash.*Done\.|Skipped\. Contents already match" "${LOG_FILE}"; then
    kill "${READER_PID}" 2>/dev/null
    echo -e "${RED}[HIL]${NC} flash phase missing -- log tail:" >&2
    tail -20 "${LOG_FILE}" >&2
    exit 1
  fi
  echo -e "${YELLOW}[HIL]${NC} flash OK"

  if ((RELAY_PROVISION_READY == 1)); then
    (
      relay_deadline=$((SECONDS + TIMEOUT_S))
      while ((SECONDS < relay_deadline)); do
        if grep -qaF "ra8_net_provision: READY v1" "$UART_LOG" 2>/dev/null; then
          printf '%s\n' "ra8_net_provision: READY v1"
          exit 0
        fi
        kill -0 "$READER_PID" 2>/dev/null || exit 1
        sleep 0.1
      done
      exit 1
    ) &
    READY_RELAY_PID=$!
  fi

  if ((PROVISION_WIFI == 1)); then
    PROVISION_READY=0
    PROVISION_DEADLINE=$((SECONDS + TIMEOUT_S))
    while ((SECONDS < PROVISION_DEADLINE)); do
      if grep -qaF "ra8_net_provision: READY v1" "$UART_LOG" 2>/dev/null; then
        PROVISION_READY=1
        break
      fi
      sleep 0.1
    done
    if ((PROVISION_READY == 0)); then
      echo -e "${RED}[HIL]${NC} runtime Wi-Fi provisioning prompt was not observed" >&2
      exit 1
    fi
    if ! python3 "$_hil_dir/../secrets/wifi_provision.py" \
      --timeout "$PROVISION_PROVIDER_TIMEOUT_S" emit |
      python3 "$_hil_dir/uart_write.py" --baud "$BAUD" "$UART"; then
      echo -e "${RED}[HIL]${NC} runtime Wi-Fi provisioning failed" >&2
      exit 1
    fi
    echo -e "${YELLOW}[HIL]${NC} runtime Wi-Fi configuration sent"
  fi

  # ---- 4. Wait for the expected string on UART  -----------------------------
  # The background reader started before flashing has been capturing into
  # ${UART_LOG} the whole time; tail-follow it until we see EXPECT or timeout.
  echo -e "${YELLOW}[HIL]${NC} waiting for '${EXPECT}' on ${UART} (${TIMEOUT_S}s)..."

  RESULT="TIMEOUT"
  # Poll the log file every 100 ms for up to TIMEOUT_S seconds.  This is
  # more robust than `tail -F | grep` which had a race where small one-shot
  # prints (12-26 bytes) sat in cat's stdio buffer and were not visible to
  # grep until cat was killed and flushed.
  deadline=$((SECONDS + TIMEOUT_S))
  while ((SECONDS < deadline)); do
    if grep -qF "${EXPECT}" "${UART_LOG}" 2>/dev/null; then
      RESULT="MATCH"
      break
    fi
    sleep 0.1
  done
  echo "--- captured UART ---"
  sed 's/\r/\\r/g' "${UART_LOG}" | head -20 | sed 's/^/[uart] /'
  echo "--- end ---"

  # Stop the background tty reader. It will keep running otherwise, consuming
  # data from the console and breaking the next test's reader. By pid only: the
  # old `pkill -f "cat $UART"` safety net also killed anyone else's reader, and
  # setsid does not make our own pid any harder to signal.
  kill "${READER_PID}" 2>/dev/null || true
  wait "${READER_PID}" 2>/dev/null || true

  # Negative-expect scan -- runs even on positive match. The point is
  # to catch firmware that emits BOTH a healthy boot banner and an
  # error banner later in the same run; a positive-only test would
  # wrongly pass.
  if [[ -n "$EXPECT_NEG" ]]; then
    NEG_HIT=$(grep -iE -- "$EXPECT_NEG" "${UART_LOG}" | head -3 || true)
    if [[ -n "$NEG_HIT" ]]; then
      echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: matched --expect-negative='${EXPECT_NEG}'"
      while IFS= read -r line; do echo "    + $line"; done <<<"$NEG_HIT"
      exit 1
    fi
  fi

  if [[ "${RESULT}" == "MATCH" ]]; then
    echo -e "${GREEN}[HIL PASS]${NC} ${APP_NAME}: saw '${EXPECT}'"
    exit 0
  else
    echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: '${EXPECT}' not seen within ${TIMEOUT_S}s"
    exit 1
  fi
else
  [[ "$-" == *p* ]]
fi

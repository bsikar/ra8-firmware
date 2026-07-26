#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_tapo.sh -- Hard power control for the HIL rig's two Tapo smart plugs.
#
#   board -- powers the EK-RA8D2 target board.  The board plug sits on the Pi's
#            local network segment, so this target is driven ON the Pi: locally
#            when invoked on the Pi, otherwise uploaded + run over SSH.
#   pi    -- powers the Raspberry Pi HIL host itself.  Driven DIRECTLY from this
#            machine so the Pi can be power-cycled even when it is offline (the
#            plug must therefore be reachable from this workstation).
#
# Usage (run from repo root):
#   bash scripts/hil/tapo.sh <board|pi> [status|on|off|cycle]
#
# Exit codes:
#   0  -- command succeeded
#   1  -- tapo_control.py error
#   2  -- usage error, Pi unreachable, or missing .env

set -euo pipefail

# Rig config (PI_HOST) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
  echo -e "${RED}[ERROR]${NC} usage: $0 <board|pi> [status|on|off|cycle]"
  exit 2
}

TARGET="${1:-}"
CMD="${2:-status}"

case "$TARGET" in
  board | pi) ;;
  *) usage ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ENV_FILE="$ROOT/.env"
TAPO_SCRIPT="$ROOT/scripts/hil/tapo_control.py"
SECRETS_SCRIPT="$ROOT/scripts/hil/hil_secrets.py"

[[ -f "$TAPO_SCRIPT" ]] ||
  {
    echo -e "${RED}[ERROR]${NC} $TAPO_SCRIPT not found"
    exit 2
  }

# Run tapo_control.py on this machine against the named plug.
run_local() {
  [[ -f "$ENV_FILE" || -f "${HOME}/.tapo.env" ]] ||
    {
      echo -e "${RED}[ERROR]${NC} neither $ENV_FILE nor ~/.tapo.env found"
      exit 2
    }
  local py="python3"
  [[ -x "$ROOT/.venv/bin/python3" ]] && py="$ROOT/.venv/bin/python3"
  echo -e "${YELLOW}[hil_tapo]${NC} ${TARGET} plug, running locally: $CMD"
  (cd "$ROOT" && "$py" "$TAPO_SCRIPT" "$TARGET" "$CMD")
}

# True when we are executing on the Pi HIL host itself.
on_pi() {
  rig_is_local_pi
}

if [[ "$TARGET" == "pi" ]]; then
  # The Pi plug must be driven directly from this machine -- never via the Pi,
  # which may be the very thing we are rebooting.
  run_local
elif on_pi; then
  # board plug, and we are already on the Pi: drive it locally.
  run_local
else
  # board plug from a dev workstation: upload + run on the Pi, whose segment is
  # the only place the board plug is reachable.
  [[ -f "$ENV_FILE" ]] ||
    {
      echo -e "${RED}[ERROR]${NC} $ENV_FILE not found -- copy .env.example and fill in values"
      exit 2
    }

  ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
    {
      echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"
      exit 2
    }

  echo -e "${YELLOW}[hil_tapo]${NC} board plug, uploading tapo_control.py..."
  REMOTE_DIR="/tmp/hil_tapo_$$"
  # shellcheck disable=SC2029  # $REMOTE_DIR is chosen locally; the Pi cannot know it.
  ssh "$PI_HOST" "mkdir -p $REMOTE_DIR"
  scp -q "$TAPO_SCRIPT" "$SECRETS_SCRIPT" "$ENV_FILE" "$PI_HOST:$REMOTE_DIR/"

  echo -e "${YELLOW}[hil_tapo]${NC} board plug, running: $CMD"
  # shellcheck disable=SC2029  # $REMOTE_DIR and $CMD are local values naming the remote dir and the action.
  ssh "$PI_HOST" "cd $REMOTE_DIR && python3 tapo_control.py board $CMD; rm -rf $REMOTE_DIR"
fi

echo -e "${GREEN}[hil_tapo DONE]${NC} ${TARGET} $CMD"

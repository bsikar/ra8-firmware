#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_tapo.sh -- Hard power control for the EK-RA8D2 board via the Tapo TP15
# smart plug on the HIL rig.  Uploads scripts/tapo_control.py and the .env
# credentials to the Pi and runs it there (the Tapo is only reachable from
# the Pi's local network segment).
#
# Usage (run from repo root on dev machine):
#   bash scripts/hil_tapo.sh [status|on|off|cycle]
#
# Exit codes:
#   0  -- command succeeded
#   1  -- tapo_control.py error
#   2  -- usage error, Pi unreachable, or missing .env

set -euo pipefail

PI_HOST="star@star.local"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

CMD="${1:-status}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="$ROOT/.env"
TAPO_SCRIPT="$ROOT/scripts/tapo_control.py"

[[ -f "$TAPO_SCRIPT" ]] \
    || { echo -e "${RED}[ERROR]${NC} $TAPO_SCRIPT not found"; exit 2; }

# Run locally if we are on the Pi (self-hosted CI runner case); otherwise
# upload + ssh to the Pi from the developer workstation.
if [[ "$(hostname 2>/dev/null || true)" == "star" ]]; then
    # tapo_control.py looks for .env in the repo root, then falls back to
    # ~/.tapo.env. We just need one of them.
    [[ -f "$ENV_FILE" || -f "${HOME}/.tapo.env" ]] \
        || { echo -e "${RED}[ERROR]${NC} neither $ENV_FILE nor ~/.tapo.env found"; exit 2; }
    echo -e "${YELLOW}[hil_tapo]${NC} running locally: $CMD"
    (cd "$ROOT" && python3 "$TAPO_SCRIPT" "$CMD")
else
    [[ -f "$ENV_FILE" ]] \
        || { echo -e "${RED}[ERROR]${NC} $ENV_FILE not found -- copy .env.example and fill in values"; exit 2; }

    ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null \
        || { echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"; exit 2; }

    echo -e "${YELLOW}[hil_tapo]${NC} uploading tapo_control.py..."
    REMOTE_DIR="/tmp/hil_tapo_$$"
    ssh "$PI_HOST" "mkdir -p $REMOTE_DIR"
    scp -q "$TAPO_SCRIPT" "$ENV_FILE" "$PI_HOST:$REMOTE_DIR/"

    echo -e "${YELLOW}[hil_tapo]${NC} running: $CMD"
    ssh "$PI_HOST" "cd $REMOTE_DIR && python3 tapo_control.py $CMD; rm -rf $REMOTE_DIR"
fi

echo -e "${GREEN}[hil_tapo DONE]${NC} $CMD"

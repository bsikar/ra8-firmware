# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

# Show workflow harness help menu
default:
    @echo "WORK - Plans and Canonical Workspace Tracking"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just work::doctor"
    @echo "  just work::board_focus                 # read-only"
    @echo "  just work::board_quick_wins            # read-only"
    @echo "  just work::board_track <track>         # read-only"
    @echo "  just work::board_epic <epic>           # read-only"
    @echo "  just work::board_issue <id>            # read-only"
    @echo "  just work::plan <notes>"
    @echo "  just work::plan_summary <notes>"
    @echo "  just work::plan_commands <notes>"
    @echo "  just work::plan_json <notes> <output>"
    @echo "  just work::start <id> [ref] [ws_root]"
    @echo "  just work::start_execute <id> [ref] [ws_root]"
    @echo "  just work::status [ws_root]"
    @echo "  just work::ready <id> [ws_root]"
    @echo "  just work::landed <id> [ws_root]"
    @echo "  just work::test"

# Report local and emitted-script readiness; the auth probe may read the network
doctor:
    python3 -I scripts/dev/work/src/work_just.py doctor

# Validate a notes file and print the default deterministic summary
plan notes:
    python3 -I scripts/dev/work/src/work_just.py plan {{ quote(notes) }} default ''

# Validate a notes file and print its deterministic summary
plan_summary notes:
    python3 -I scripts/dev/work/src/work_just.py plan {{ quote(notes) }} summary ''

# Validate a notes file and print a human-reviewed GitHub mutation script
plan_commands notes:
    python3 -I scripts/dev/work/src/work_just.py plan {{ quote(notes) }} commands ''

# Validate a notes file and atomically write deterministic JSON
plan_json notes output:
    python3 -I scripts/dev/work/src/work_just.py plan {{ quote(notes) }} json {{ quote(output) }}

# Preview the canonical workspace for a task identifier
start id ref="HEAD" ws_root="":
    python3 -I scripts/dev/work/src/work_just.py start {{ quote(id) }} {{ quote(ref) }} {{ quote(ws_root) }} false

# Create the canonical workspace through the locked lifecycle authority
start_execute id ref="HEAD" ws_root="":
    python3 -I scripts/dev/work/src/work_just.py start {{ quote(id) }} {{ quote(ref) }} {{ quote(ws_root) }} true

# List work-owned canonical records; forged reserved records make this fail
status ws_root="":
    python3 -I scripts/dev/work/src/work_just.py status {{ quote(ws_root) }}

# Run exact local CI for a clean committed claim before the one authorized push
ready id ws_root="":
    python3 -I scripts/dev/work/src/work_just.py ready {{ quote(id) }} {{ quote(ws_root) }}

# Require content-equivalent origin/dev and cached remote PASS after the push
landed id ws_root="":
    python3 -I scripts/dev/work/src/work_just.py landed {{ quote(id) }} {{ quote(ws_root) }}

# Run the canonical registered gate, including real-Just argv preservation
test:
    "{{ just_executable() }}" quality::gate::run work-harness

# Show active, owner-blocked, high-priority, and bench-blocked work without mutation
board_focus:
    python3 -I scripts/dev/work/src/work_just.py board_focus

# Show explicit Ready + effort:S candidates without claiming they are closable
board_quick_wins:
    python3 -I scripts/dev/work/src/work_just.py board_quick_wins

# Filter open project issues by configured Track without mutation
board_track track:
    python3 -I scripts/dev/work/src/work_just.py board_track {{ quote(track) }}

# Filter open project issues by an explicit epic label without mutation
board_epic epic:
    python3 -I scripts/dev/work/src/work_just.py board_epic {{ quote(epic) }}

# Inspect one open project issue and its native relationships without mutation
board_issue id:
    python3 -I scripts/dev/work/src/work_just.py board_issue {{ quote(id) }}

# Move an issue to a different status column on the Kanban board
board_move id status:
    python3 -I scripts/dev/work/src/work_just.py board_move {{ quote(id) }} {{ quote(status) }}

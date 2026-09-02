# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

# HIL is a hardware/privilege boundary. Do not carry a caller-selected Python
# environment, startup hook, or PATH into the first image/bench check.

export BASH_ENV := "/dev/null"
export ENV := "/dev/null"
export PYTHONHOME := ""
export PYTHONPATH := ""
export PYTHONNOUSERSITE := "1"
export RA8_TOOL_VENV := ""
export PATH := ".venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# Show HIL help menu
default:
    @echo "HIL - guarded local and remote EK-RA8D2 bench operations"
    @echo ""
    @echo "SUITES AND PROGRAMMING:"
    @echo "  just hil::run [apps] [wait]                   Build locally and run on the remote rig"
    @echo "  just hil::suite [app] [directory] [skip]      Run the suite in the current checkout"
    @echo "  just hil::c6 [app] [wait]                     Run the ESP32-C6 hardware lane"
    @echo "  just hil::flash <app> [wait]                  Build and flash on the remote rig"
    @echo "  just hil::flash_retry <app> [wait]            Power-cycle, then retry a flash"
    @echo "  just hil::recover <app> [wait]                Recovery flash for a wedged target"
    @echo "  just hil::reflash <app> [wait]                Reset DLM/RoT state, then flash"
    @echo "  just hil::run_local <app> [uart] [wait]       Run one HIL gate on a local macOS rig"
    @echo "  just hil::erase [wait]                        Mass-erase target MRAM"
    @echo "  just hil::dlm_reset [wait]                    Reset device lifecycle state"
    @echo "  just hil::dlm_check_local                     Inspect a locally attached target read-only"
    @echo "  just hil::dlm_recover_local                   Confirm and reset a locally attached target"
    @echo "  just hil::preflash_check <image>              Validate an image without programming it"
    @echo "  just hil::probe [wait]                        Read-only J-Link/target diagnostic"
    @echo "  just hil::find_jlink                          Find J-Links on the configured or local rig"
    @echo ""
    @echo "BENCH LOCK:"
    @echo "  just hil::status                              Show holder (0 free, 1 held, 3 unknown)"
    @echo "  just hil::hold \"why\" [duration] [detached] Hold for an interactive shell"
    @echo "  just hil::free                                Release a detached hold you own"
    @echo "  just hil::extend <duration>                   Extend your detached hold budget"
    @echo "  just hil::take \"reason\" [duration] [confirm] Journaled preemption"
    @echo "  just hil::log [lines]                         Tail the bench audit journal"
    @echo "  just hil::doctor                              Validate the lock deployment"
    @echo "  just hil::selftest                            Prove lock and SSH-death behavior"
    @echo "  just hil::contention                          Exercise real multi-host contention"
    @echo ""
    @echo "RIG UTILITIES:"
    @echo "  just hil::camera_tunnel [ip] [port]           Forward the rig camera UI"
    @echo "  just hil::camera_video [frames] [fps] [file]  Record guarded rig-camera frames as MP4"
    @echo "  just hil::tapo [target] [action] [wait]       Control a declared smart plug"
    @echo "  just hil::ppps <off|on|cycle> [port]          Control one USB hub port"
    @echo "  just hil::remote_gdb [run|stop] [port] [app]  IDE-owned remote J-Link GDB tunnel"

# Build the selected HIL apps locally, stage them, and run on the remote rig.
run apps="" wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    args=()
    [ -n "{{ apps }}" ] && args=(--only "{{ apps }}")
    /bin/bash -p scripts/hil/dev.sh "${args[@]}"

# Run the HIL suite from the current checkout (normally on the bench host).
suite app="" directory="examples/ek_ra8d2/hw_validated/hil" skip_build="0" wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    args=(--dir "{{ directory }}")
    [ -n "{{ app }}" ] && args+=(--only "{{ app }}")
    [ "{{ skip_build }}" = "1" ] && args+=(--skip-build)
    /bin/bash -p scripts/hil/all.sh "${args[@]}"

# Run the ESP32-C6 lane, which requires its documented SW4/harness topology.
c6 app="" wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    args=(--dir examples/ek_ra8d2/hw_validated/c6)
    [ -n "{{ app }}" ] && args+=(--only "{{ app }}")
    /bin/bash -p scripts/hil/all.sh "${args[@]}"

# Build and flash an app to the Pi-attached board over SSH.
flash app wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    "{{ just_executable() }}" apps::build "{{ app }}"
    /bin/bash -p scripts/hil/flash.sh "{{ app }}"

# Power-cycle the target, then retry programming it.
flash_retry app wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    "{{ just_executable() }}" apps::build "{{ app }}"
    /bin/bash -p scripts/hil/flash_retry.sh "{{ app }}"

# Recovery flash for a target that ordinary programming cannot reach.
recover app wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    "{{ just_executable() }}" apps::build "{{ app }}"
    /bin/bash -p scripts/hil/recover.sh "{{ app }}"

# Reset TrustZone/DLM/RoT state and flash a clean image under one bench hold.
reflash app wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    "{{ just_executable() }}" apps::build "{{ app }}"
    /bin/bash -p scripts/hil/reflash.sh "{{ app }}"

# Run one app's HIL verifier against a board attached to this macOS host.
run_local app uart="" wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    args=("{{ app }}")
    [ -n "{{ uart }}" ] && args+=(--uart "{{ uart }}")
    /bin/bash -p scripts/hil/run_local.sh "${args[@]}"

# Mass-erase target MRAM.
erase wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    /bin/bash -p scripts/hil/erase.sh

# Probe J-Link and board connectivity without programming firmware.
probe wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    /bin/bash -p scripts/hil/probe.sh

# Reset DLM (Device Lifecycle Management) state on the board.
dlm_reset wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    /bin/bash -p scripts/hil/dlm_reset.sh

# Inspect a board attached directly to this host without modifying it.
dlm_check_local:
    /bin/bash -p scripts/hil/dlm_reset_local.sh check

# Recover a directly attached board; this mass-erases target flash.
[confirm("Mass-erase this locally attached target and reset its DLM state?")]
dlm_recover_local:
    /bin/bash -p scripts/hil/dlm_reset_local.sh recover

# Validate an ELF/image with the common guard without touching the target.
preflash_check image:
    #!/bin/bash -p
    set -euo pipefail
    image={{ quote(image) }}
    source scripts/hil/lib/preflash_guard.sh
    ra8_preflash_guard "$image"

# Find J-Link serials / USB IDs without touching target memory.
find_jlink:
    /bin/bash -p scripts/hil/find_jlink.sh

# Show the remote bench lock status.
status:
    /bin/bash -p scripts/hil/bench.sh status

# Prove the lock mechanism, including release after an SSH client dies.
selftest:
    /bin/bash -p scripts/hil/bench.sh selftest --ssh-death

# Hold the bench for an interactive shell, or explicitly request a detached hold.
hold why="" duration="" detached="0":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ why }}" ] || { echo 'usage: just hil::hold "description" [duration] [detached: 0|1]' >&2; exit 2; }
    args=(hold --why "{{ why }}")
    [ -n "{{ duration }}" ] && args+=(--for "{{ duration }}")
    [ "{{ detached }}" = "1" ] && args+=(--detached)
    /bin/bash -p scripts/hil/bench.sh "${args[@]}"

# Release a detached hold owned by this actor.
free:
    /bin/bash -p scripts/hil/bench.sh free

# Extend the budget on a detached hold.
extend duration="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ duration }}" ] || { echo 'usage: just hil::extend 30m' >&2; exit 2; }
    /bin/bash -p scripts/hil/bench.sh extend --for "{{ duration }}"

# Journaled preemption; taking from a human also requires the confirmation phrase.
take reason="" duration="" confirm="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ reason }}" ] || { echo 'usage: just hil::take "reason" [duration] [confirmation]' >&2; exit 2; }
    args=(take --reason "{{ reason }}")
    [ -n "{{ duration }}" ] && args+=(--for "{{ duration }}")
    [ -n "{{ confirm }}" ] && export CONFIRM="{{ confirm }}"
    /bin/bash -p scripts/hil/bench.sh "${args[@]}"

# Tail the append-only bench journal.
log lines="25":
    /bin/bash -p scripts/hil/bench.sh log "{{ lines }}"

# Check the deployed state directory, inode, permissions, clock, and SSH liveness bound.
doctor:
    /bin/bash -p scripts/hil/bench.sh doctor

# Prove exclusion and liveness under real multi-host contention.
contention phase="" rounds="" victim="" wait="":
    #!/bin/bash -p
    set -euo pipefail
    args=()
    [ -n "{{ phase }}" ] && args+=(--phase "{{ phase }}")
    [ -n "{{ rounds }}" ] && args+=(--rounds "{{ rounds }}")
    [ -n "{{ victim }}" ] && args+=(--victim "{{ victim }}")
    [ -n "{{ wait }}" ] && args+=(--wait "{{ wait }}")
    /bin/bash -p scripts/hil/bench_contention.sh "${args[@]}"

# Forward the rig-only camera UI to localhost.
camera_tunnel ip="" port="8080":
    #!/bin/bash -p
    set -euo pipefail
    args=()
    [ -n "{{ ip }}" ] && args+=("{{ ip }}")
    [ -n "{{ port }}" ] && args+=("{{ port }}")
    /bin/bash -p scripts/hil/camera_tunnel.sh "${args[@]}"

# Record guarded rig-camera frames and encode them as an MP4.
camera_video frames="3" fps="1" output="camera_capture.mp4" wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    frames={{ quote(frames) }}
    fps={{ quote(fps) }}
    output={{ quote(output) }}
    /bin/bash -p scripts/hil/camera_video.sh --frames "$frames" --fps "$fps" --output "$output"

# Control one declared smart plug. The default is read-only status.
tapo target="board" action="status" wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    /bin/bash -p scripts/hil/tapo.sh "{{ target }}" "{{ action }}"

# Control a USB hub port. There is intentionally no destructive default action.
ppps action="" port="2" soft="0" wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ action }}" ] || { echo 'usage: just hil::ppps <off|on|cycle> [port] [soft: 0|1]' >&2; exit 2; }
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    args=()
    [ "{{ soft }}" = "1" ] && args+=(--soft)
    args+=("{{ action }}" "{{ port }}")
    /bin/bash -p scripts/hil/ppps.sh "${args[@]}"

# Start or stop the guarded SSH tunnel used by IDE remote-debug configurations.
remote_gdb action="run" port="2331" app="" wait="":
    #!/bin/bash -p
    set -euo pipefail
    [ -n "{{ wait }}" ] && export RA8_BENCH_WAIT="{{ wait }}"
    action={{ quote(action) }}
    port={{ quote(port) }}
    app={{ quote(app) }}
    args=("$action" "$port")
    [ -n "$app" ] && args+=("$app")
    /bin/bash -p scripts/dev/remote_gdb_server.sh "${args[@]}"

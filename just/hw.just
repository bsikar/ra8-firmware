# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show hardware help menu
default:
    @echo "HARDWARE - Local Bench Flashing, Debugging & Serial"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::hardware::flash <app>             Flash app to local board using J-Link"
    @echo "  just apps::hardware::flash <app> ocd=1       Flash app to local board using OpenOCD"
    @echo "  just apps::hardware::debug <app>             Start GDB debug session with J-Link"
    @echo "  just apps::hardware::debug <app> ocd=1       Start GDB debug session with OpenOCD"
    @echo "  just apps::hardware::ozone <app>             Launch SEGGER Ozone GUI debugger"
    @echo "  just apps::hardware::monitor                 Open live UART serial console"
    @echo "  just apps::hardware::monitor app=<app>       Build, flash app, and stream serial"
    @echo "  just apps::hardware::list                    List all apps capable of being flashed"
    @echo "  just apps::hardware::search <keyword>        Search flashable apps by keyword"
    @echo ""

# List all flashable apps
list:
    "{{ just_executable() }}" apps::example::list

# Search flashable apps
search keyword="":
    "{{ just_executable() }}" apps::example::search "{{ keyword }}"

# Build and flash an app to hardware attached to THIS machine
flash app ocd="0":
    #!/bin/bash -p
    # SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
    set -euo pipefail
    "{{ just_executable() }}" apps::build {{ app }}
    app_dir="$(python3 scripts/dev/ra8_apps.py dir {{ app }})"
    app_name="$(python3 scripts/dev/ra8_apps.py name {{ app }})"
    hex="$app_dir/build/$app_name.hex"
    if [ "{{ ocd }}" = "1" ]; then
        /bin/bash -p scripts/dev/openocd_flash.sh "$hex"
    else
        /bin/bash -p scripts/dev/flash.sh "$hex"
    fi

# Build and start GDB debug session on attached hardware
debug app ocd="0":
    #!/bin/bash -p
    # SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
    set -euo pipefail
    "{{ just_executable() }}" apps::build {{ app }}
    app_dir="$(python3 scripts/dev/ra8_apps.py dir {{ app }})"
    app_name="$(python3 scripts/dev/ra8_apps.py name {{ app }})"
    elf="$app_dir/build/$app_name.elf"
    if [ "{{ ocd }}" = "1" ]; then
        /bin/bash -p scripts/dev/openocd_debug.sh "$elf"
    else
        /bin/bash -p scripts/dev/debug.sh "$elf"
    fi

# Build and launch SEGGER Ozone GUI debugger on attached hardware
ozone app:
    #!/bin/bash -p
    # SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
    set -euo pipefail
    "{{ just_executable() }}" apps::build {{ app }}
    app_dir="$(python3 scripts/dev/ra8_apps.py dir {{ app }})"
    app_name="$(python3 scripts/dev/ra8_apps.py name {{ app }})"
    elf="$app_dir/build/$app_name.elf"
    /bin/bash -p scripts/dev/ozone.sh "$elf"

# Open live UART serial console stream (optionally build + flash app first)
monitor app="":
    #!/bin/bash -p
    # SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
    set -euo pipefail
    if [ -n "{{ app }}" ]; then
        "{{ just_executable() }}" apps::hardware::flash {{ app }}
    fi
    /bin/bash -p scripts/dev/monitor.sh

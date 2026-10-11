# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The emulator is bsikar/ra8-emulator, pinned at tools/ra8_emulator. It builds
# with Zig (`zig build`) and never links SDL; the windowed debugger is the
# separate `ra8_gui` binary, built by the same tree with -Dgui.

set working-directory := ".."

# Show emulator help menu
default:
    @echo "EMULATOR - ra8-emulator (tools/ra8_emulator submodule)"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::emulator::run <app>         Build an app and run it on the emulator (headless)"
    @echo "  just apps::emulator::gui [app]         Open the ra8_gui windowed debugger on a built app"
    @echo "  just apps::emulator::profile <app>     Profile an app and write folded stacks"
    @echo "  just apps::emulator::list              List all apps capable of being emulated"
    @echo "  just apps::emulator::search <keyword>  Search emulatable apps by keyword"
    @echo ""
    @echo "Every other ra8_emulator flag (SD cards, frame dumps, tracing, consoles,"
    @echo "instruction budgets) is available directly:"
    @echo "  tools/ra8_emulator/zig-out/bin/ra8_emulator --help"
    @echo ""

# Build the ra8-emulator binaries; kind="gui" also builds ra8_gui (-Dgui)
build_emu kind="run":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -f tools/ra8_emulator/build.zig ]; then
        echo "tools/ra8_emulator is a submodule (bsikar/ra8-emulator) and is not checked out." >&2
        echo "Run 'git submodule update --init tools/ra8_emulator' and retry." >&2
        exit 1
    fi
    flags=(-Doptimize=ReleaseFast)
    if [ "{{ kind }}" = "gui" ]; then
        flags+=(-Dgui)
    fi
    (cd tools/ra8_emulator && zig build "${flags[@]}")

# List all emulatable apps
list:
    "{{ just_executable() }}" apps::example::list

# Search emulatable apps
search keyword="":
    "{{ just_executable() }}" apps::example::search "{{ keyword }}"

# Build an app and run it on the emulator (headless; a window is apps::emulator::gui)
run app="ereader_ui":
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" apps::emulator::build_emu
    app_name="$(python3 scripts/dev/ra8_apps.py name "{{ app }}")"

    if [ "$app_name" = "ra8d2-ereader" ]; then
        dir="apps/board/stand_alone/ereader"
        build_dir="$dir/build-emu"
        cmake -S "$dir" -B "$build_dir" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        cmake --build "$build_dir" -j
        tools/ra8_emulator/zig-out/bin/ra8_emulator "$build_dir/ra8d2-ereader.elf" --ns "$build_dir/ra8d2-ereader_ns.elf"
    elif [ "$app_name" = "dualcore_mailbox" ]; then
        dir="$(python3 scripts/dev/ra8_apps.py dir dualcore_mailbox)"
        build_dir="$dir/build-emu"
        cmake -S "$dir" -B "$build_dir" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        cmake --build "$build_dir" -j
        tools/ra8_emulator/zig-out/bin/ra8_emulator "$build_dir/dualcore_mailbox.elf"
    elif [ "$app_name" = "tz_threadx_demo" ]; then
        dir="$(python3 scripts/dev/ra8_apps.py dir tz_threadx_demo)"
        build_dir="$dir/build-emu"
        cmake -S "$dir" -B "$build_dir" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        cmake --build "$build_dir" -j
        tools/ra8_emulator/zig-out/bin/ra8_emulator "$build_dir/tz_threadx_demo.elf" --ns "$build_dir/tz_threadx_demo_ns.elf"
    else
        dir="$(python3 scripts/dev/ra8_apps.py dir "{{ app }}")"
        "{{ just_executable() }}" apps::build "{{ app }}"
        tools/ra8_emulator/zig-out/bin/ra8_emulator "$dir/build/$app_name.elf"
    fi

# Open the windowed debugger (ra8_gui) on a built app
gui app="ereader_shelf":
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" apps::emulator::build_emu gui
    app_dir="$(python3 scripts/dev/ra8_apps.py dir "{{ app }}")"
    app_name="$(python3 scripts/dev/ra8_apps.py name "{{ app }}")"
    "{{ just_executable() }}" apps::build "{{ app }}"
    tools/ra8_emulator/zig-out/bin/ra8_gui "$app_dir/build/$app_name.elf"

# Profile an app and write folded stacks (pipe build/<app>.folded into flamegraph.pl)
profile app="ereader_ui":
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" apps::emulator::build_emu
    "{{ just_executable() }}" apps::build "{{ app }}"
    app_dir="$(python3 scripts/dev/ra8_apps.py dir "{{ app }}")"
    app_name="$(python3 scripts/dev/ra8_apps.py name "{{ app }}")"
    mkdir -p build
    tools/ra8_emulator/zig-out/bin/ra8_emulator "$app_dir/build/$app_name.elf" --profile --profile-folded "build/$app_name.folded"
    echo "folded profile: build/$app_name.folded"

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show emulator help menu
default:
    @echo "EMULATOR - macOS Unicorn CPU Simulation & Profiling"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::emulator::run <app>               Run app in emulator with live window view"
    @echo "  just apps::emulator::gui                     Launch interactive E-Reader desktop GUI"
    @echo "  just apps::emulator::setup                   Install macOS host dependencies"
    @echo "  just apps::emulator::matrix                  Run headless emulator matrix test suite"
    @echo "  just apps::emulator::matrix_triage           Triage failures from last matrix run"
    @echo "  just apps::emulator::matrix_baseline         Update matrix ratchet baseline"
    @echo "  just apps::emulator::eil [app] [jobs]        Run Emulated Hardware-In-The-Loop tests"
    @echo "  just apps::emulator::golden                  Check e-reader chrome golden images"
    @echo "  just apps::emulator::golden_update           Update e-reader chrome golden images"
    @echo "  just apps::emulator::profile <app>           Generate flamechart execution profile"
    @echo "  just apps::emulator::list                    List all apps capable of being emulated"
    @echo "  just apps::emulator::search <keyword>        Search emulatable apps by keyword"
    @echo ""

# List all emulatable apps
list:
    "{{ just_executable() }}" apps::example::list

# Search emulatable apps
search keyword="":
    "{{ just_executable() }}" apps::example::search "{{ keyword }}"

# Install macOS emulator dependencies (Homebrew packages, Python dependencies)
setup:
    /bin/bash -p scripts/emu/setup_macos.sh

# Run an app in local macOS Unicorn CPU emulator
run app="ereader_ui" panel="ek_ra8d2":
    #!/usr/bin/env bash
    set -euo pipefail
    # Ensure emulator is built
    "{{ just_executable() }}" tools::build_one ra8_emulator
    app_name="$(python3 scripts/dev/ra8_apps.py name "{{ app }}")"

    if [ "$app_name" = "ra8d2-ereader" ]; then
        dir="apps/board/stand_alone/ereader"
        build_dir="$dir/build-emu"
        cmake -S "$dir" -B "$build_dir" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        cmake --build "$build_dir" -j
        tools/ra8_emulator/build/ra8_emulator "$build_dir/ra8d2-ereader.elf" --ns "$build_dir/ra8d2-ereader_ns.elf" --panel "tools/ra8_emulator/panels/{{ panel }}.toml" --view
    elif [ "$app_name" = "dualcore_mailbox" ]; then
        dir="$(python3 scripts/dev/ra8_apps.py dir dualcore_mailbox)"
        build_dir="$dir/build-emu"
        cmake -S "$dir" -B "$build_dir" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        cmake --build "$build_dir" -j
        tools/ra8_emulator/build/ra8_emulator "$build_dir/dualcore_mailbox.elf" --panel "tools/ra8_emulator/panels/{{ panel }}.toml" --view
    elif [ "$app_name" = "tz_threadx_demo" ]; then
        dir="$(python3 scripts/dev/ra8_apps.py dir tz_threadx_demo)"
        build_dir="$dir/build-emu"
        cmake -S "$dir" -B "$build_dir" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        cmake --build "$build_dir" -j
        tools/ra8_emulator/build/ra8_emulator "$build_dir/tz_threadx_demo.elf" --ns "$build_dir/tz_threadx_demo_ns.elf" --panel "tools/ra8_emulator/panels/{{ panel }}.toml" --view
    else
        dir="$(python3 scripts/dev/ra8_apps.py dir "{{ app }}")"
        "{{ just_executable() }}" apps::build "{{ app }}"
        tools/ra8_emulator/build/ra8_emulator "$dir/build/$app_name.elf" --panel "tools/ra8_emulator/panels/{{ panel }}.toml" --view
    fi

# Launch full hybrid e-reader GUI with virtual SD card
gui panel="ek_ra8d2" count="5" fast_sd="1":
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" apps::build ereader_shelf
    "{{ just_executable() }}" tools::build_one ra8_emulator
    "{{ just_executable() }}" tools::build_one mkbookimg
    mkdir -p build
    sd_dir="apps/board/stand_alone/ereader/content/compiled"
    shopt -s nullglob
    all=("$sd_dir"/*.rabook)
    books=("${all[@]:0:{{ count }}}")
    if [ ${#books[@]} -eq 0 ]; then
        echo "no *.rabook in $sd_dir; generating stub book..."
        # run books generator if available
    fi
    tools/mkbookimg/build/mkbookimg build/ereader_sd.img "${books[@]}"
    fast_flag=""
    [ "{{ fast_sd }}" = "1" ] && fast_flag="--fast-sd"
    app_dir="$(python3 scripts/dev/ra8_apps.py dir ereader_shelf)"
    tools/ra8_emulator/build/ra8_emulator "$app_dir/build/ereader_shelf.elf" --sd build/ereader_sd.img $fast_flag --panel "tools/ra8_emulator/panels/{{ panel }}.toml" --view

# Run full headless emulator test matrix
matrix:
    bash scripts/emu/matrix.sh

# Group the last matrix sweep's failures by cause
matrix_triage:
    bash scripts/emu/matrix_triage.sh

alias matrix-triage := matrix_triage

# Update the emulator matrix ratchet baseline
matrix_baseline:
    bash scripts/emu/matrix.sh --baseline

alias matrix-baseline := matrix_baseline

# Run EIL (Emulated Hardware-In-The-Loop), optionally for one app / worker count
eil app="" jobs="":
    #!/usr/bin/env bash
    set -euo pipefail
    args=()
    if [ -n "{{ jobs }}" ]; then
        args+=(-j "{{ jobs }}")
    fi
    if [ -n "{{ app }}" ]; then
        args+=(--only "{{ app }}")
    fi
    /bin/bash -p scripts/emu/eil_all.sh "${args[@]}"

# Check e-reader chrome against the committed emulator framebuffer goldens
golden:
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" apps::build ereader_ui
    "{{ just_executable() }}" tools::build_one ra8_emulator
    python3 scripts/gen/ereader_golden.py check \
        --elf "$(python3 scripts/dev/ra8_apps.py dir ereader_ui)/build/ereader_ui.elf" \
        --emulator tools/ra8_emulator/build/ra8_emulator \
        --golden-dir tests/golden/ereader_chrome \
        --out-dir /tmp/ereader_golden_out

# Regenerate the committed e-reader chrome emulator framebuffer goldens
golden_update:
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" apps::build ereader_ui
    "{{ just_executable() }}" tools::build_one ra8_emulator
    python3 scripts/gen/ereader_golden.py update \
        --elf "$(python3 scripts/dev/ra8_apps.py dir ereader_ui)/build/ereader_ui.elf" \
        --emulator tools/ra8_emulator/build/ra8_emulator \
        --golden-dir tests/golden/ereader_chrome

# Profile an app with interactive flamechart GUI
profile app mode="full" stop_pc="" profile_args="":
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" apps::build "{{ app }}"
    "{{ just_executable() }}" tools::build_one ra8_emulator
    app_dir="$(python3 scripts/dev/ra8_apps.py dir "{{ app }}")"
    app_name="$(python3 scripts/dev/ra8_apps.py name "{{ app }}")"
    RA8_EMU_PROFILE={{ mode }} RA8_EMU_STOP_PC={{ stop_pc }} RA8_EMU_MAX_CHUNKS=8000000 \
        tools/ra8_emulator/build/ra8_emulator "$app_dir/build/$app_name.elf" {{ profile_args }}
    if [ -f ra8_emulator_profile.html ]; then
        echo "Opening flamechart GUI: ra8_emulator_profile.html"
        if command -v open >/dev/null 2>&1; then
            if ! open ra8_emulator_profile.html; then
                echo "Could not open the flamechart; open ra8_emulator_profile.html manually" >&2
            fi
        elif command -v xdg-open >/dev/null 2>&1; then
            if ! xdg-open ra8_emulator_profile.html; then
                echo "Could not open the flamechart; open ra8_emulator_profile.html manually" >&2
            fi
        else
            echo "No supported browser opener found; open ra8_emulator_profile.html manually" >&2
        fi
    fi

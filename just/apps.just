# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

mod shared 'shared.just'
mod host 'host.just'
mod board 'board.just'
mod test 'app_test.just'
mod emulator 'emu.just'
mod hardware 'hw.just'
mod filter 'filter.just'
mod example 'example.just'

# Show apps help menu and category overview
default:
    @echo "APP - Firmware Applications and Examples"
    @echo ""
    @echo "SUBMODULES:"
    @echo "  just apps::shared                            App-level shared libraries (portable)"
    @echo "  just apps::host                              Standalone desktop applications"
    @echo "  just apps::board                             On-target standalone board applications"
    @echo "  just apps::test                              Host application unit & integration tests"
    @echo "  just apps::emulator                          macOS CPU simulation and profiling"
    @echo "  just apps::hardware                          Local bench hardware flash and debug"
    @echo "  just apps::filter                            Pre-configured firmware application queues"
    @echo "  just apps::example                           Firmware examples and reference code"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::clean                             Remove all build output directories"
    @echo "  just apps::compile_commands                  Regenerate compile_commands.json database"
    @echo ""

# Build a single firmware application
build app="" build_type="RelWithDebInfo":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ app }}" ]; then
        echo "Error: Please specify a firmware application to build."
        echo ""
        python3 scripts/dev/ra8_apps.py list --all
        exit 1
    fi
    if [ "{{ app }}" = "all" ]; then
        echo "Error: 'just apps::build all' has moved. Use 'just apps::example::build all' or 'just build_all'."
        exit 1
    fi
    app_dir="$(python3 scripts/dev/ra8_apps.py dir "{{ app }}")"
    toolchain="$(python3 scripts/dev/ra8_apps.py toolchain "{{ app }}")"
    build_dir="$(python3 scripts/dev/ra8_apps.py build-dir "{{ app }}")"
    cmake_args=()
    while IFS= read -r -d '' argument; do
        cmake_args+=("$argument")
    done < <(python3 scripts/dev/ra8_apps.py cmake-args "{{ app }}")
    # CMake caches absolute source paths; discard only an incompatible cache
    # when switching between native macOS and the /workspace devcontainer.
    # shellcheck source=scripts/builders/select_host_compiler.sh
    . scripts/builders/select_host_compiler.sh
    ra8_cmake_reset_if_incompatible "$build_dir" "$app_dir"
    mkdir -p "$build_dir"
    unset RA8_C6_WIFI_SSID RA8_C6_WIFI_PSK RA8_MEDIA_DOWNLOAD_URL
    cmake -S "$app_dir" -B "$build_dir" \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/$toolchain" \
      -DCMAKE_BUILD_TYPE="{{ build_type }}" \
      -U 'RA8_C6_WIFI_*' -U RA8_MEDIA_DOWNLOAD_URL \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "${cmake_args[@]}"
    cmake --build "$build_dir"

# Remove all build output directories
clean:
    #!/usr/bin/env python3
    import os, shutil, glob
    print("Cleaning all build directories...")
    for p in glob.glob("apps/**/build", recursive=True) + glob.glob("apps/**/build-*", recursive=True) + glob.glob("examples/**/build", recursive=True) + glob.glob("examples/**/build-*", recursive=True):
        if os.path.isdir(p) and not p.startswith(".git/"):
            print(f"  rm -rf {p}")
            shutil.rmtree(p, ignore_errors=True)

# Regenerate the repository-root compile_commands.json across all apps
compile_commands:
    python3 scripts/builders/build_cross_compile_db.py -o . --check

alias compile-commands := compile_commands

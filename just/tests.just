# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

default:
    @echo "TESTS - Host Unit & Integration Tests"
    @echo ""
    @echo "SUBMODULES:"
    @echo "  just tests::local <name|all>                 Run a specific test natively on macOS/Linux"
    @echo "  just tests::devcontainer <name|all>          Run a specific test inside isolated Linux container"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just tests::build                            Build the test suite"
    @echo "  just tests::clean                            Clean the test suite build directory"

    @echo ""
    @echo "CATEGORIES:"
    @echo "  just tests::hal                              Hardware Abstraction Layer (HAL) drivers"
    @echo "  just tests::storage                          Storage, Filesystem (FS), and Flash Translation Layer (FTL)"
    @echo "  just tests::usb                              USB Device, Host, and Class drivers (CDC, MSC, HID)"
    @echo "  just tests::wireless                         ESP32-C6 Wi-Fi, BLE, and Modem abstraction"
    @echo "  just tests::security                         Security, Crypto, RSIP, and Key Vault"
    @echo "  just tests::net                              Networking, LwIP, and Ethernet"
    @echo ""
    @echo "  just tests::shared                           App-Level Shared Libraries (Portable)"
    @echo "  just tests::host                             Standalone Desktop Applications"
    @echo "  just tests::board                            On-Target Standalone Board Applications"
    @echo "  just tests::tools                            Desktop Developer Tools and Codecs"

build:
    bash tests/build_tests.sh

clean:
    rm -rf tests/build*

mod local 'test_local.just'
mod devcontainer 'test_devcontainer.just'

# --- Category Listings ---

hal:
    python3 scripts/dev/list_tests.py hal

storage:
    python3 scripts/dev/list_tests.py storage

usb:
    python3 scripts/dev/list_tests.py usb

wireless:
    python3 scripts/dev/list_tests.py wireless

security:
    python3 scripts/dev/list_tests.py security

net:
    python3 scripts/dev/list_tests.py net

# --- Host App Tests ---

# Build and run mdl unit tests
mdl:
    bash scripts/builders/host_cmake.sh apps/host/mdl apps/host/mdl/build
    ctest --test-dir apps/host/mdl/build --output-on-failure

# Build and run alphabet_soup unit tests
alphabet_soup:
    bash scripts/builders/host_cmake.sh apps/host/alphabet_soup apps/host/alphabet_soup/build
    ctest --test-dir apps/host/alphabet_soup/build --output-on-failure

# Run integration tests combining mdl, mkbookimg, rabook_viewer, and ra8_reader
integration:
    bash scripts/builders/host_cmake.sh apps/host/mdl apps/host/mdl/build -DBUILD_INTEGRATION_TESTS=ON
    ctest --test-dir apps/host/mdl/build -R test_integration --output-on-failure

shared:
    python3 scripts/dev/list_tests.py shared

host:
    python3 scripts/dev/list_tests.py host

board:
    python3 scripts/dev/list_tests.py board

tools:
    python3 scripts/dev/list_tests.py tools

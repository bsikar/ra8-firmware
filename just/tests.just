# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

default:
    @echo "TESTS - Host Unit & Integration Tests"
    @echo ""
    @echo "SUBMODULES:"
    @echo "  just tests::local          Explore native test commands"
    @echo "  just tests::devcontainer   Explore isolated Linux test commands"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just tests::build          Build the test suite"
    @echo "  just tests::clean          Clean the test suite build directory"

    @echo ""
    @echo "CATEGORIES:"
    @echo "  just tests::hal            Hardware Abstraction Layer (HAL) drivers"
    @echo "  just tests::storage        Storage, Filesystem (FS), and Flash Translation Layer (FTL)"
    @echo "  just tests::usb            USB Device, Host, and Class drivers (CDC, MSC, HID)"
    @echo "  just tests::wireless       ESP32-C6 Wi-Fi, BLE, and Modem abstraction"
    @echo "  just tests::security       Security, Crypto, RSIP, and Key Vault"
    @echo "  just tests::net            Networking, LwIP, and Ethernet"
    @echo "  just tests::alphabet_soup  Run the alphabet-soup test lane"
    @echo ""
    @echo "  just tests::shared         App-Level Shared Libraries (Portable)"
    @echo "  just tests::host           Standalone Desktop Applications"
    @echo "  just tests::board          On-Target Standalone Board Applications"
    @echo "  just tests::tools          Desktop Developer Tools and Codecs"

build:
    bash tests/build_tests.sh

clean:
    rm -rf tests/build*

mod local 'test_local.just'
mod devcontainer 'test_devcontainer.just'

# --- Category Listings ---

hal:
    bash scripts/builders/list_tests.sh hal

storage:
    bash scripts/builders/list_tests.sh storage

usb:
    bash scripts/builders/list_tests.sh usb

wireless:
    bash scripts/builders/list_tests.sh wireless

security:
    bash scripts/builders/list_tests.sh security

net:
    bash scripts/builders/list_tests.sh net

# --- Host App Tests ---

# Build and run alphabet_soup unit tests
alphabet_soup:
    bash scripts/builders/host_cmake.sh apps/host/alphabet_soup apps/host/alphabet_soup/build
    ctest --test-dir apps/host/alphabet_soup/build --output-on-failure

shared:
    bash scripts/builders/list_tests.sh shared

host:
    bash scripts/builders/list_tests.sh host

board:
    bash scripts/builders/list_tests.sh board

tools:
    bash scripts/builders/list_tests.sh tools

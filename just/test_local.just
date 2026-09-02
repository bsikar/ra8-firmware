# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default name="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ name }}" ]; then
        echo "Error: Please specify a test name or regex (or 'all')." >&2
        echo "Usage: just tests::local <name|all>" >&2
        exit 2
    fi
    "{{ just_executable() }}" tests::build

    REGEX="{{ name }}"

    # Allow users to pass namespaced targets (e.g. hal::test_ra8_adc) by stripping the prefix
    REGEX="${REGEX##*::}"

    if [ "${REGEX}" = "all" ] || [ "{{ name }}" = "all" ]; then
        bash tests/run_tests.sh
        exit $?
    elif [ "${REGEX}" = "hal" ]; then
        REGEX="test_ra8_(adc|dac|gpt|i2c|i3c|spi|sci|can|ether|usb_dcd|usb_hcd|sdhi|xspi|mipi|glcdc|pdg|pcntr|iwdt|wdt|lpm|rtc|cgc|fcu|gpio|icu|ipc|isr|mstp|pdm|poeg|pwr|reset|scb|systick|time|timer)"
    elif [ "${REGEX}" = "storage" ]; then
        REGEX="test_ra8_(fs|ftl|io_blockdev|io_vfs|sdmmc|sdcard|sram)"
    elif [ "${REGEX}" = "usb" ]; then
        REGEX="test_ra8_usb"
    elif [ "${REGEX}" = "wireless" ]; then
        REGEX="test_ra8_(wifi|modem|tls)"
    elif [ "${REGEX}" = "security" ]; then
        REGEX="test_ra8_(rsip|psa|secure|tz|key|pin|rot|root_of_trust)"
    elif [ "${REGEX}" = "net" ]; then
        REGEX="test_ra8_(net|layer3|nsc|rmac|tls_net)"
    elif [ "${REGEX}" = "shared" ]; then
        echo "==> Delegating to apps::shared::test all"
        "{{ just_executable() }}" apps::shared::test all
        exit $?
    elif [ "${REGEX}" = "host" ]; then
        echo "==> Delegating to apps::test::run all"
        "{{ just_executable() }}" apps::test::run all
        exit $?
    elif [ "${REGEX}" = "tools" ]; then
        echo "==> Tools tests are tested via the CI tools-build gate."
        "{{ just_executable() }}" quality::local::gate tools-build
        exit $?
    elif [ "${REGEX}" = "board" ]; then
        echo "==> Board apps are tested via the HIL suite."
        "{{ just_executable() }}" hil::run
        exit $?
    fi

    bash tests/run_tests.sh -R "$REGEX"

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_ota -- Phase-5 OTA firmware-update orchestration.
#
# Provides the state machine that drives:
#   1. HTTPS manifest fetch (via injected ra8_ota_net_iface_t).
#   2. Streamed download of the firmware blob into the inactive MRAM
#      bank with inline SHA-256.
#   3. ECDSA signature verification against the manifest digest.
#   4. Boot-bank swap (ra8_flash_set_startup_area) + NVIC reset.
#
# The module never calls malloc -- all buffers live in static storage
# inside libs/ra8_ota/src/ra8_ota.c. The network and crypto backends are
# injected through function-pointer interfaces (see ra8_ota.h) so this
# library can be unit-tested without ra8_tls or mbedtls present.
#
# Picked up automatically by tests/CMakeLists.txt globbing. No board-
# side firmware example currently links it directly, so this file
# documents scope and lets a future example wire it up via target_*.
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.

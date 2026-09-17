# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Transport-independent OV5640 sensor driver. Consumers list `ra8_ov5640` in
# ra8_add_app(LIBS ...); the app helper and host-test build discover inc/ and
# src/ directly, matching the other first-party sensor/domain libraries.

# Intentionally empty -- consumers reach into inc/ + src/ directly.

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_jpeg -- platform-neutral baseline JPEG codec.
#
# This is image-domain logic: it has no MMIO, board, RTOS, filesystem, or
# allocation dependency. Consumers compile inc/ + src/ directly, matching the
# other reusable Domain libraries. JOF pulls it transitively in
# cmake/ra8_app/sources.cmake; direct consumers list `ra8_jpeg` in LIBS.

# Intentionally empty -- consumers reach into inc/ + src/ directly.

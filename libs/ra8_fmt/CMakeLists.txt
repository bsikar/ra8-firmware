# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# libs/ra8_fmt/CMakeLists.txt
#
# Intentionally a no-op, like ra8_c6link / ra8_io, and with no src/
# yet: this library is currently the header-only home of the format-neutral
# byte-stream contract (ra8_fmt_io.h), so a consumer only ever needs
# libs/ra8_fmt/inc on its include path.
#
# It is the landing site for #755. The ra8_fmt engines still live under
# tools/rabook_imagepack/src and are not consumable from a product yet; as they
# are promoted they gain a src/ here and this file grows the add_library()
# target the issue asks for. The host-edge adapters (ra8_fmt_host_fd.c,
# ra8_fmt_host_spool.c) stay in tools/ by design: they are the POSIX binding.

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# libs/ra8_mdl/CMakeLists.txt
#
# Intentionally a no-op, like ra8_c6link / ra8_io / ra8_display_pal, and with
# no src/ at all: this library is the header-only home of the transport-neutral
# media HTTP contract (ra8_mdl_http.h), so a consumer only ever needs
# libs/ra8_mdl/inc on its include path. Every consumer of libs/ra8_c6link/inc
# needs this beside it, because ra8_mdl_protocol.h and ra8_mdl_request.h are
# both built on the contract declared here.

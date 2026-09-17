# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Source-only first-party library. Firmware consumers list this adapter plus
# `if`, `ra8_c6link`, and whichever composition-root binding supplies the
# `fw_fs_transaction_port_t` (today `if_ra8_vfs`) in ra8_add_app(LIBS ...).
# It compiles no filesystem backend of its own: the bound port does that.

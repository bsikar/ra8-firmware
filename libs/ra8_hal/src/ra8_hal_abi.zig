//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of libra8_hal.a, the Zig half of ra8_hal. It exports nothing
//! itself: each ported unit's C ABI lives in src/<unit>_abi.zig and is
//! compiled as its own archive member (RA8FW-542), so the linker pulls in
//! only the units an image calls, and only their log strings. The
//! prototypes in inc/ are unchanged and stay the membrane.

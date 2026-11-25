//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `libs/ra8_translate`: aggregates C ABI boundary adapters.

comptime {
    _ = @import("ra8_translate_abi");
    _ = @import("ctc_decode_abi");
    _ = @import("detector_post_abi");
}

//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one decidable question on the copy-to-run path: may this hand-off
//! copy anything at all. The copy itself writes the SRAM run base and
//! branches into it, so it cannot run off target; this part can, and is
//! where both pre-copy guards live.

const image = @import("image");

/// Whether a hand-off may proceed to the copy.
///
/// `entry` is cross-checked against the trusted run base rather than used:
/// the copy always goes to `image.layout.run_base`, so a corrupted `entry`
/// cannot redirect it, it only fails here and the launch returns.
pub fn mayCopy(src: usize, img_len: u32, entry: u32) bool {
    return src != 0 and image.runTargetValid(entry, img_len);
}

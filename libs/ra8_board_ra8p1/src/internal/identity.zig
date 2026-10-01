//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Board identity strings. Held as slices here; the ABI layer is what hands C
//! the null-terminated pointers.

/// Board name, doc revision and MCU part number.
pub const Board = struct {
    pub const name = "RA8P1 foundation board";
    pub const doc_rev = "R01UH1064EJ (chip HUM)";
    pub const mcu = "R7KA8P1KFLCAC";
};

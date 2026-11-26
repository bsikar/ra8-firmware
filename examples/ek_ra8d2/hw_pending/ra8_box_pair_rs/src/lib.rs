// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Paired `#![no_std]` Rust client of the `ra8_box` Zig C ABI (`libs/ra8_box/inc/ra8_box.h`),
//! exercising the shared behavior vectors in `tests/zig_abi_pairs/ra8_box_pair/vectors.json`.

#![cfg_attr(target_os = "none", no_std)]

pub mod ffi;
pub mod firmware;
pub mod vectors;

pub use ffi::{Ra8Box, Ra8BoxTree, Ra8UiRect};
pub use firmware::format_banner;
pub use vectors::{
    CONSTANTS, Constants, ERR_VECTORS, ErrOp, ErrVec, K_RA8_BOX_GRID, K_RA8_BOX_LEAF,
    K_RA8_BOX_NO_COLOUR, K_RA8_BOX_NONE, K_RA8_BOX_STACK_H, K_RA8_BOX_STACK_V,
    K_RA8_ERR_INVALID_ARG, K_RA8_ERR_NULL_PTR, K_RA8_OK, LAYOUT_VECTORS, LayoutVec,
    MAX_NODES_PER_CASE, NodeVec, PairResult, run_all,
};

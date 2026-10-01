//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J35 camera pin map: the 8-bit DVP bus the CEU drives, plus the three
//! single-purpose pins (XCLK, reset) the rest of the adapter owns.
//! EK-RA8D2 UM Table 35 p 48.

const vocab = @import("vocab.zig");

const Pin = vocab.Pin;

/// CAM D0, P400.
pub const d0: u16 = Pin.pack(4, 0);
/// CAM D1, P902.
pub const d1: u16 = Pin.pack(9, 2);
/// CAM D2, P405 (J41).
pub const d2: u16 = Pin.pack(4, 5);
/// CAM D3, P406 (J41).
pub const d3: u16 = Pin.pack(4, 6);
/// CAM D4, P700.
pub const d4: u16 = Pin.pack(7, 0);
/// CAM D5, P701.
pub const d5: u16 = Pin.pack(7, 1);
/// CAM D6, P702.
pub const d6: u16 = Pin.pack(7, 2);
/// CAM D7, P703.
pub const d7: u16 = Pin.pack(7, 3);
/// CAM VSYNC, PB02.
pub const vsync: u16 = Pin.pack(11, 2);
/// CAM HSYNC, PB03.
pub const hsync: u16 = Pin.pack(11, 3);
/// CAM PCLK, PB04.
pub const pclk: u16 = Pin.pack(11, 4);
/// CAM XCLK, P501. Driven by the GPT, not the CEU.
pub const xclk: u16 = Pin.pack(5, 1);
/// CAM RST, P709. Plain GPIO.
pub const rst: u16 = Pin.pack(7, 9);

/// Every pin the CEU itself drives, in the order the C routed them.
pub const parallel = [_]u16{ d0, d1, d2, d3, d4, d5, d6, d7, vsync, hsync, pclk };

/// Owner string recorded against each CEU route.
pub const parallel_owner = "board.camera.ceu";
/// Owner string recorded against the XCLK route.
pub const xclk_owner = "board.camera.xclk";

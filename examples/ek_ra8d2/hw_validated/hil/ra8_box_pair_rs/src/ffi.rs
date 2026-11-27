// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Audited C ABI membrane for `libs/ra8_box/inc/ra8_box.h` and EK-RA8D2 board
//! bringup APIs. All `unsafe` declarations and FFI calls are confined here.

#![expect(
    unsafe_code,
    reason = "C ABI declarations and audited FFI calls to ra8_box and EK-RA8D2 board APIs"
)]

use core::ptr::{null, null_mut};

/// Layout rectangle (`ra8_ui_rect_t`).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(C)]
pub struct Ra8UiRect {
    /// Origin X in display coordinates.
    pub x: i32,
    /// Origin Y in display coordinates.
    pub y: i32,
    /// Width in pixels.
    pub w: i32,
    /// Height in pixels.
    pub h: i32,
}

/// Box-tree node (`ra8_box_t`).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(C)]
pub struct Ra8Box {
    /// Box layout kind (`ra8_box_kind_t`).
    pub kind: u8,
    /// Grid columns when `kind == grid`.
    pub grid_cols: u8,
    /// Fixed main-axis extent; `0` selects `flex`.
    pub fixed: i16,
    /// Flex weight when `fixed == 0`.
    pub flex: u16,
    /// Inner padding on all four sides.
    pub pad: i16,
    /// Gap between adjacent children.
    pub gap: i16,
    /// Background colour or `k_ra8_box_no_colour`.
    pub fill: u32,
    /// Border colour or `k_ra8_box_no_colour`.
    pub border: u32,
    /// Border width in pixels.
    pub border_w: i16,
    /// Application action tag.
    pub tag: i16,
    /// First child index or `k_ra8_box_none`.
    pub first_child: i32,
    /// Next sibling index or `k_ra8_box_none`.
    pub next: i32,
    /// Output laid-out rectangle computed by `ra8_box_layout`.
    pub rect: Ra8UiRect,
}

impl Ra8Box {
    /// Return a zeroed node matching C aggregate zero-initialization (`= {}`).
    #[must_use]
    pub const fn zeroed() -> Self {
        Self {
            kind: 0,
            grid_cols: 0,
            fixed: 0,
            flex: 0,
            pad: 0,
            gap: 0,
            fill: 0,
            border: 0,
            border_w: 0,
            tag: 0,
            first_child: 0,
            next: 0,
            rect: Ra8UiRect {
                x: 0,
                y: 0,
                w: 0,
                h: 0,
            },
        }
    }
}

/// Append-only box-tree builder (`ra8_box_tree_t`).
#[repr(C)]
pub struct Ra8BoxTree {
    /// Caller-owned backing array of box nodes.
    pub nodes: *mut Ra8Box,
    /// Total node capacity in `nodes`.
    pub cap: u16,
    /// Number of nodes appended so far.
    pub count: u16,
}

impl Default for Ra8BoxTree {
    fn default() -> Self {
        Self::empty()
    }
}

impl Ra8BoxTree {
    /// Return an uninitialized empty tree matching `= {}` in C23.
    #[must_use]
    pub const fn empty() -> Self {
        Self {
            nodes: null_mut(),
            cap: 0,
            count: 0,
        }
    }
}

const _: () = {
    assert!(size_of::<Ra8UiRect>() == 16);
    assert!(align_of::<Ra8UiRect>() >= 4);
    assert!(size_of::<Ra8Box>() == 48);
    assert!(align_of::<Ra8Box>() >= 4);
};

unsafe extern "C" {
    fn ra8_box_tree_init(tree: *mut Ra8BoxTree, storage: *mut Ra8Box, cap: u16) -> u16;
    fn ra8_box_add(tree: *mut Ra8BoxTree, parent: i16, node: *const Ra8Box) -> i16;
    fn ra8_box_layout(tree: *mut Ra8BoxTree, root: i16, frame: *const Ra8UiRect) -> u16;
}

/// Bind `tree` to caller-owned `storage` (nullable arguments exercise null-ptr guards).
pub fn box_tree_init(
    tree: Option<&mut Ra8BoxTree>,
    storage: Option<&mut [Ra8Box]>,
    cap: u16,
) -> u16 {
    let tree_ptr = tree.map_or(null_mut(), core::ptr::from_mut);
    let storage_ptr = storage.map_or(null_mut(), <[Ra8Box]>::as_mut_ptr);
    unsafe { ra8_box_tree_init(tree_ptr, storage_ptr, cap) }
}

/// Append `node` as a child of `parent` (nullable arguments exercise null-ptr guards).
pub fn box_add(tree: Option<&mut Ra8BoxTree>, parent: i16, node: Option<&Ra8Box>) -> i16 {
    let tree_ptr = tree.map_or(null_mut(), core::ptr::from_mut);
    let node_ptr = node.map_or(null(), core::ptr::from_ref);
    unsafe { ra8_box_add(tree_ptr, parent, node_ptr) }
}

/// Lay out `tree` starting at `root` into `frame` (nullable arguments exercise null-ptr guards).
pub fn box_layout(tree: Option<&mut Ra8BoxTree>, root: i16, frame: Option<&Ra8UiRect>) -> u16 {
    let tree_ptr = tree.map_or(null_mut(), core::ptr::from_mut);
    let frame_ptr = frame.map_or(null(), core::ptr::from_ref);
    unsafe { ra8_box_layout(tree_ptr, root, frame_ptr) }
}

#[cfg(target_os = "none")]
pub mod board {
    //! EK-RA8D2 substrate prologue, console write, LED toggle, IRQ enable, and delay.

    /// Selector bit for LED1 (`k_ra8_board_bringup_led1`).
    pub const BRINGUP_LED1_MASK: u32 = 0x1;
    /// LED1 identifier (`k_ra8_board_led1`).
    pub const LED1_ID: u8 = 0;

    /// Substrate prologue configuration (`ra8_board_bringup_cfg_t`).
    #[repr(C)]
    pub struct BringupCfg {
        /// J-Link OB VCOM baud rate (`0` disables console).
        pub console_baud: u32,
        /// `ra8_board_bringup_led_mask_t` bitmask.
        pub leds_mask: u32,
        /// Whether to enable global interrupts at the end of bringup.
        pub enable_interrupts: bool,
    }

    /// Observed clock rates (`ra8_board_bringup_out_t`).
    #[repr(C)]
    pub struct BringupOut {
        /// Live CPUCLK0 rate in Hz.
        pub cpuclk0_hz: u32,
        /// Live PCLKA rate in Hz.
        pub pclka_hz: u32,
    }

    unsafe extern "C" {
        fn ra8_board_bringup(cfg: *const BringupCfg, out: *mut BringupOut) -> u16;
        fn ra8_board_uart_console_write(data: *const u8, len: usize) -> u16;
        fn ra8_board_led_toggle(led: u8) -> u16;
        fn ra8_isr_globals_enable();
        fn ra8_delay_ms(ms: u32);
    }

    /// Bring up clocks, timebase, SCI8 console, and LED1.
    #[must_use]
    pub fn bringup(console_baud: u32) -> u16 {
        let cfg = BringupCfg {
            console_baud,
            leds_mask: BRINGUP_LED1_MASK,
            enable_interrupts: false,
        };
        let mut out = BringupOut {
            cpuclk0_hz: 0,
            pclka_hz: 0,
        };
        unsafe { ra8_board_bringup(&raw const cfg, &raw mut out) }
    }

    /// Enable maskable CPU interrupts.
    pub fn enable_global_irqs() {
        unsafe { ra8_isr_globals_enable() };
    }

    /// Write raw bytes to the SCI8 debug console.
    pub fn uart_write(bytes: &[u8]) -> u16 {
        unsafe { ra8_board_uart_console_write(bytes.as_ptr(), bytes.len()) }
    }

    /// Toggle user LED `led`.
    #[must_use]
    pub fn led_toggle(led: u8) -> u16 {
        unsafe { ra8_board_led_toggle(led) }
    }

    /// Busy-wait `ms` milliseconds on the SysTick timebase.
    pub fn delay_ms(ms: u32) {
        unsafe { ra8_delay_ms(ms) };
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Freestanding vector table and executor matching
//! `tests/zig_abi_pairs/ra8_box_pair/vectors.json`.

use crate::ffi::{Ra8Box, Ra8BoxTree, Ra8UiRect, box_add, box_layout, box_tree_init};

/// Sentinel linking no child or no sibling (`k_ra8_box_none`).
pub const K_RA8_BOX_NONE: i32 = -1;
/// Absent background or border colour (`k_ra8_box_no_colour`).
pub const K_RA8_BOX_NO_COLOUR: u32 = 0;
/// Vertical stack layout kind (`k_ra8_box_stack_v`).
pub const K_RA8_BOX_STACK_V: u8 = 0;
/// Horizontal stack layout kind (`k_ra8_box_stack_h`).
pub const K_RA8_BOX_STACK_H: u8 = 1;
/// Row-major fixed-column grid layout kind (`k_ra8_box_grid`).
pub const K_RA8_BOX_GRID: u8 = 2;
/// Leaf box kind (`k_ra8_box_leaf`).
pub const K_RA8_BOX_LEAF: u8 = 3;
/// Success status code (`k_ra8_ok`).
pub const K_RA8_OK: u16 = 0;
/// Invalid-argument status code (`k_ra8_err_invalid_arg`).
pub const K_RA8_ERR_INVALID_ARG: u16 = 259;
/// Null-pointer status code (`k_ra8_err_null_ptr`).
pub const K_RA8_ERR_NULL_PTR: u16 = 1_284;
/// Maximum node storage needed for any single shared layout vector.
pub const MAX_NODES_PER_CASE: usize = 8;

/// Shared contract constants mirrored from `vectors.json`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Constants {
    /// Expected `k_ra8_box_none`.
    pub k_ra8_box_none: i32,
    /// Expected `k_ra8_box_no_colour`.
    pub k_ra8_box_no_colour: i32,
    /// Expected `k_ra8_box_stack_v`.
    pub k_ra8_box_stack_v: u8,
    /// Expected `k_ra8_box_stack_h`.
    pub k_ra8_box_stack_h: u8,
    /// Expected `k_ra8_box_grid`.
    pub k_ra8_box_grid: u8,
    /// Expected `k_ra8_box_leaf`.
    pub k_ra8_box_leaf: u8,
    /// Expected `k_ra8_ok`.
    pub k_ra8_ok: u16,
    /// Expected `k_ra8_err_invalid_arg`.
    pub k_ra8_err_invalid_arg: u16,
    /// Expected `k_ra8_err_null_ptr`.
    pub k_ra8_err_null_ptr: u16,
    /// Expected `sizeof(ra8_ui_rect_t)`.
    pub sizeof_ra8_ui_rect_t: usize,
    /// Expected `sizeof(ra8_box_t)`.
    pub sizeof_ra8_box_t: usize,
    /// Expected `max_nodes_per_case`.
    pub max_nodes_per_case: u16,
}

/// Canonical ABI and contract constants verified on host against `vectors.json`.
pub const CONSTANTS: Constants = Constants {
    k_ra8_box_none: K_RA8_BOX_NONE,
    k_ra8_box_no_colour: 0,
    k_ra8_box_stack_v: K_RA8_BOX_STACK_V,
    k_ra8_box_stack_h: K_RA8_BOX_STACK_H,
    k_ra8_box_grid: K_RA8_BOX_GRID,
    k_ra8_box_leaf: K_RA8_BOX_LEAF,
    k_ra8_ok: K_RA8_OK,
    k_ra8_err_invalid_arg: K_RA8_ERR_INVALID_ARG,
    k_ra8_err_null_ptr: K_RA8_ERR_NULL_PTR,
    sizeof_ra8_ui_rect_t: size_of::<Ra8UiRect>(),
    sizeof_ra8_box_t: size_of::<Ra8Box>(),
    max_nodes_per_case: 8,
};

/// Operation selector for negative / guard vectors.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrOp {
    /// Invoke `ra8_box_tree_init`.
    TreeInit,
    /// Invoke `ra8_box_add`.
    Add,
    /// Invoke `ra8_box_layout`.
    Layout,
}

/// One shared guard / error behavior vector.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ErrVec {
    /// Stable identifier from `vectors.json`.
    pub id: &'static str,
    /// Target C ABI function under test.
    pub op: ErrOp,
    /// Pass a null `ra8_box_tree_t*`.
    pub null_tree: bool,
    /// Pass a null secondary pointer (`storage` / `node` / `frame`).
    pub null_arg: bool,
    /// Storage capacity passed to `ra8_box_tree_init`.
    pub cap: u16,
    /// Leaf nodes seeded before invoking the checked operation.
    pub seed_count: u16,
    /// `parent` (for `add`) or `root` (for `layout`) index argument.
    pub index_arg: i16,
    /// Expected `ra8_err_t` result.
    pub expected_err: u16,
    /// Expected `int16_t` index result (or `-1`).
    pub expected_idx: i16,
}

/// Expected input and computed layout state for one node in a `LayoutVec`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NodeVec {
    /// Parent index when adding (`k_ra8_box_none` for root).
    pub parent: i16,
    /// Input box template.
    pub input: Ra8Box,
    /// Expected return value of `ra8_box_add`.
    pub expected_index: i16,
    /// Expected `first_child` after building the tree.
    pub expected_first_child: i32,
    /// Expected `next` sibling after building the tree.
    pub expected_next: i32,
    /// Expected computed `rect` after `ra8_box_layout`.
    pub expected_rect: Ra8UiRect,
}

/// One shared box-layout tree vector.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LayoutVec {
    /// Stable identifier from `vectors.json`.
    pub id: &'static str,
    /// Outer root rectangle passed to `ra8_box_layout`.
    pub frame: Ra8UiRect,
    /// Root node index passed to `ra8_box_layout`.
    pub root: i16,
    /// Ordered nodes added and verified.
    pub nodes: &'static [NodeVec],
}

/// Aggregate execution result matching `ra8_box_pair_result_t`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PairResult {
    /// Total vectors executed.
    pub total: u16,
    /// Vectors that passed.
    pub passed: u16,
    /// Vectors that failed.
    pub failed: u16,
    /// Identifier of the first vector that failed, if any.
    pub failed_id: Option<&'static str>,
}

const fn rect(x: i32, y: i32, w: i32, h: i32) -> Ra8UiRect {
    Ra8UiRect { x, y, w, h }
}

/// Shared error / guard behavior vectors (`13` cases).
pub const ERR_VECTORS: [ErrVec; 13] = [
    ErrVec {
        id: "tree_init_null_tree",
        op: ErrOp::TreeInit,
        null_tree: true,
        null_arg: false,
        cap: 4,
        seed_count: 0,
        index_arg: 0,
        expected_err: K_RA8_ERR_NULL_PTR,
        expected_idx: -1,
    },
    ErrVec {
        id: "tree_init_null_storage",
        op: ErrOp::TreeInit,
        null_tree: false,
        null_arg: true,
        cap: 4,
        seed_count: 0,
        index_arg: 0,
        expected_err: K_RA8_ERR_NULL_PTR,
        expected_idx: -1,
    },
    ErrVec {
        id: "tree_init_zero_cap",
        op: ErrOp::TreeInit,
        null_tree: false,
        null_arg: false,
        cap: 0,
        seed_count: 0,
        index_arg: 0,
        expected_err: K_RA8_ERR_INVALID_ARG,
        expected_idx: -1,
    },
    ErrVec {
        id: "add_null_tree",
        op: ErrOp::Add,
        null_tree: true,
        null_arg: false,
        cap: 4,
        seed_count: 0,
        index_arg: -1,
        expected_err: 0,
        expected_idx: -1,
    },
    ErrVec {
        id: "add_null_node",
        op: ErrOp::Add,
        null_tree: false,
        null_arg: true,
        cap: 4,
        seed_count: 0,
        index_arg: -1,
        expected_err: 0,
        expected_idx: -1,
    },
    ErrVec {
        id: "add_bad_parent_neg",
        op: ErrOp::Add,
        null_tree: false,
        null_arg: false,
        cap: 4,
        seed_count: 1,
        index_arg: -2,
        expected_err: 0,
        expected_idx: -1,
    },
    ErrVec {
        id: "add_bad_parent_oob",
        op: ErrOp::Add,
        null_tree: false,
        null_arg: false,
        cap: 4,
        seed_count: 1,
        index_arg: 1,
        expected_err: 0,
        expected_idx: -1,
    },
    ErrVec {
        id: "add_overflow_cap",
        op: ErrOp::Add,
        null_tree: false,
        null_arg: false,
        cap: 1,
        seed_count: 1,
        index_arg: 0,
        expected_err: 0,
        expected_idx: -1,
    },
    ErrVec {
        id: "layout_null_tree",
        op: ErrOp::Layout,
        null_tree: true,
        null_arg: false,
        cap: 4,
        seed_count: 1,
        index_arg: 0,
        expected_err: K_RA8_ERR_NULL_PTR,
        expected_idx: -1,
    },
    ErrVec {
        id: "layout_null_frame",
        op: ErrOp::Layout,
        null_tree: false,
        null_arg: true,
        cap: 4,
        seed_count: 1,
        index_arg: 0,
        expected_err: K_RA8_ERR_NULL_PTR,
        expected_idx: -1,
    },
    ErrVec {
        id: "layout_empty_tree",
        op: ErrOp::Layout,
        null_tree: false,
        null_arg: false,
        cap: 4,
        seed_count: 0,
        index_arg: 0,
        expected_err: K_RA8_ERR_INVALID_ARG,
        expected_idx: -1,
    },
    ErrVec {
        id: "layout_bad_root_neg",
        op: ErrOp::Layout,
        null_tree: false,
        null_arg: false,
        cap: 4,
        seed_count: 1,
        index_arg: -1,
        expected_err: K_RA8_ERR_INVALID_ARG,
        expected_idx: -1,
    },
    ErrVec {
        id: "layout_bad_root_oob",
        op: ErrOp::Layout,
        null_tree: false,
        null_arg: false,
        cap: 4,
        seed_count: 1,
        index_arg: 1,
        expected_err: K_RA8_ERR_INVALID_ARG,
        expected_idx: -1,
    },
];

const VEC0_NODES: [NodeVec; 4] = [
    NodeVec {
        parent: -1,
        input: Ra8Box {
            kind: K_RA8_BOX_STACK_V,
            grid_cols: 1,
            pad: 10,
            gap: 10,
            fill: 0xFF11_2233,
            border: 0xFF22_3344,
            border_w: 1,
            tag: 100,
            ..Ra8Box::zeroed()
        },
        expected_index: 0,
        expected_first_child: 1,
        expected_next: -1,
        expected_rect: rect(0, 0, 100, 200),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            fixed: 30,
            fill: 0xFFFF_0000,
            tag: 101,
            ..Ra8Box::zeroed()
        },
        expected_index: 1,
        expected_first_child: -1,
        expected_next: 2,
        expected_rect: rect(10, 10, 80, 30),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            flex: 1,
            fill: 0xFF00_FF00,
            tag: 102,
            ..Ra8Box::zeroed()
        },
        expected_index: 2,
        expected_first_child: -1,
        expected_next: 3,
        expected_rect: rect(10, 50, 80, 110),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            fixed: 20,
            fill: 0xFF00_00FF,
            tag: 103,
            ..Ra8Box::zeroed()
        },
        expected_index: 3,
        expected_first_child: -1,
        expected_next: -1,
        expected_rect: rect(10, 170, 80, 20),
    },
];

const VEC1_NODES: [NodeVec; 4] = [
    NodeVec {
        parent: -1,
        input: Ra8Box {
            kind: K_RA8_BOX_STACK_H,
            grid_cols: 1,
            pad: 10,
            gap: 10,
            tag: 200,
            ..Ra8Box::zeroed()
        },
        expected_index: 0,
        expected_first_child: 1,
        expected_next: -1,
        expected_rect: rect(10, 20, 260, 60),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            fixed: 40,
            fill: 1,
            border: 2,
            border_w: 1,
            tag: 201,
            ..Ra8Box::zeroed()
        },
        expected_index: 1,
        expected_first_child: -1,
        expected_next: 2,
        expected_rect: rect(20, 30, 40, 40),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            flex: 1,
            fill: 3,
            border: 4,
            border_w: 2,
            tag: 202,
            ..Ra8Box::zeroed()
        },
        expected_index: 2,
        expected_first_child: -1,
        expected_next: 3,
        expected_rect: rect(70, 30, 60, 40),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            flex: 2,
            fill: 5,
            border: 6,
            border_w: 3,
            tag: 203,
            ..Ra8Box::zeroed()
        },
        expected_index: 3,
        expected_first_child: -1,
        expected_next: -1,
        expected_rect: rect(140, 30, 120, 40),
    },
];

const VEC2_NODES: [NodeVec; 4] = [
    NodeVec {
        parent: -1,
        input: Ra8Box {
            kind: K_RA8_BOX_GRID,
            grid_cols: 2,
            gap: 10,
            tag: 300,
            ..Ra8Box::zeroed()
        },
        expected_index: 0,
        expected_first_child: 1,
        expected_next: -1,
        expected_rect: rect(0, 0, 100, 100),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            tag: 301,
            ..Ra8Box::zeroed()
        },
        expected_index: 1,
        expected_first_child: -1,
        expected_next: 2,
        expected_rect: rect(0, 0, 45, 45),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            fixed: 25,
            tag: 302,
            ..Ra8Box::zeroed()
        },
        expected_index: 2,
        expected_first_child: -1,
        expected_next: 3,
        expected_rect: rect(55, 0, 45, 25),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            tag: 303,
            ..Ra8Box::zeroed()
        },
        expected_index: 3,
        expected_first_child: -1,
        expected_next: -1,
        expected_rect: rect(0, 55, 45, 45),
    },
];

const VEC3_NODES: [NodeVec; 8] = [
    NodeVec {
        parent: -1,
        input: Ra8Box {
            kind: K_RA8_BOX_STACK_V,
            grid_cols: 1,
            pad: 8,
            gap: 8,
            tag: 400,
            ..Ra8Box::zeroed()
        },
        expected_index: 0,
        expected_first_child: 1,
        expected_next: -1,
        expected_rect: rect(0, 0, 320, 240),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_STACK_H,
            grid_cols: 1,
            fixed: 40,
            gap: 4,
            tag: 401,
            ..Ra8Box::zeroed()
        },
        expected_index: 1,
        expected_first_child: 3,
        expected_next: 2,
        expected_rect: rect(8, 8, 304, 40),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_GRID,
            grid_cols: 3,
            flex: 1,
            gap: 10,
            tag: 402,
            ..Ra8Box::zeroed()
        },
        expected_index: 2,
        expected_first_child: 5,
        expected_next: -1,
        expected_rect: rect(8, 56, 304, 176),
    },
    NodeVec {
        parent: 1,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            fixed: 60,
            tag: 403,
            ..Ra8Box::zeroed()
        },
        expected_index: 3,
        expected_first_child: -1,
        expected_next: 4,
        expected_rect: rect(8, 8, 60, 40),
    },
    NodeVec {
        parent: 1,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            flex: 1,
            tag: 404,
            ..Ra8Box::zeroed()
        },
        expected_index: 4,
        expected_first_child: -1,
        expected_next: -1,
        expected_rect: rect(72, 8, 240, 40),
    },
    NodeVec {
        parent: 2,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            tag: 405,
            ..Ra8Box::zeroed()
        },
        expected_index: 5,
        expected_first_child: -1,
        expected_next: 6,
        expected_rect: rect(8, 56, 94, 176),
    },
    NodeVec {
        parent: 2,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            tag: 406,
            ..Ra8Box::zeroed()
        },
        expected_index: 6,
        expected_first_child: -1,
        expected_next: 7,
        expected_rect: rect(112, 56, 94, 176),
    },
    NodeVec {
        parent: 2,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            tag: 407,
            ..Ra8Box::zeroed()
        },
        expected_index: 7,
        expected_first_child: -1,
        expected_next: -1,
        expected_rect: rect(216, 56, 94, 176),
    },
];

const VEC4_NODES: [NodeVec; 3] = [
    NodeVec {
        parent: -1,
        input: Ra8Box {
            kind: K_RA8_BOX_GRID,
            pad: 15,
            gap: 20,
            tag: 500,
            ..Ra8Box::zeroed()
        },
        expected_index: 0,
        expected_first_child: 1,
        expected_next: -1,
        expected_rect: rect(5, 5, 20, 12),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            tag: 501,
            ..Ra8Box::zeroed()
        },
        expected_index: 1,
        expected_first_child: -1,
        expected_next: 2,
        expected_rect: rect(20, 20, 0, 0),
    },
    NodeVec {
        parent: 0,
        input: Ra8Box {
            kind: K_RA8_BOX_LEAF,
            fixed: 10,
            tag: 502,
            ..Ra8Box::zeroed()
        },
        expected_index: 2,
        expected_first_child: -1,
        expected_next: -1,
        expected_rect: rect(20, 40, 0, 10),
    },
];

/// Shared layout tree behavior vectors (`5` trees, `22` nodes).
pub const LAYOUT_VECTORS: [LayoutVec; 5] = [
    LayoutVec {
        id: "stack_v_padded_fixed_flex",
        frame: rect(0, 0, 100, 200),
        root: 0,
        nodes: &VEC0_NODES,
    },
    LayoutVec {
        id: "stack_h_weighted_flex",
        frame: rect(10, 20, 260, 60),
        root: 0,
        nodes: &VEC1_NODES,
    },
    LayoutVec {
        id: "grid_2col_partial_row",
        frame: rect(0, 0, 100, 100),
        root: 0,
        nodes: &VEC2_NODES,
    },
    LayoutVec {
        id: "nested_chrome_tree",
        frame: rect(0, 0, 320, 240),
        root: 0,
        nodes: &VEC3_NODES,
    },
    LayoutVec {
        id: "clamp_underflow_zero_floor",
        frame: rect(5, 5, 20, 12),
        root: 0,
        nodes: &VEC4_NODES,
    },
];

fn seed_tree(tree: &mut Ra8BoxTree, seed_count: u16) -> bool {
    let mut leaf = Ra8Box::zeroed();
    leaf.kind = K_RA8_BOX_LEAF;
    for _ in 0..seed_count {
        if box_add(Some(tree), -1, Some(&leaf)) < 0 {
            return false;
        }
    }
    true
}

fn eval_err_vector(vec: &ErrVec) -> bool {
    let mut storage = [Ra8Box::zeroed(); MAX_NODES_PER_CASE];
    let mut tree = Ra8BoxTree::empty();
    let mut leaf = Ra8Box::zeroed();
    leaf.kind = K_RA8_BOX_LEAF;
    let frame = rect(0, 0, 10, 10);

    if vec.op != ErrOp::TreeInit {
        if box_tree_init(Some(&mut tree), Some(&mut storage), vec.cap) != K_RA8_OK {
            return false;
        }
        if !seed_tree(&mut tree, vec.seed_count) {
            return false;
        }
    }

    let before = tree.count;
    let tree_arg = if vec.null_tree { None } else { Some(&mut tree) };
    match vec.op {
        ErrOp::TreeInit => {
            let storage_arg = if vec.null_arg {
                None
            } else {
                Some(&mut storage[..])
            };
            box_tree_init(tree_arg, storage_arg, vec.cap) == vec.expected_err
        }
        ErrOp::Add => {
            let node_arg = if vec.null_arg { None } else { Some(&leaf) };
            let got = box_add(tree_arg, vec.index_arg, node_arg);
            if got != vec.expected_idx {
                return false;
            }
            if !vec.null_tree && tree.count != before {
                return false;
            }
            true
        }
        ErrOp::Layout => {
            let frame_arg = if vec.null_arg { None } else { Some(&frame) };
            box_layout(tree_arg, vec.index_arg, frame_arg) == vec.expected_err
        }
    }
}

fn check_node(actual: &Ra8Box, expected: &NodeVec) -> bool {
    actual.kind == expected.input.kind
        && actual.grid_cols == expected.input.grid_cols
        && actual.fixed == expected.input.fixed
        && actual.flex == expected.input.flex
        && actual.pad == expected.input.pad
        && actual.gap == expected.input.gap
        && actual.fill == expected.input.fill
        && actual.border == expected.input.border
        && actual.border_w == expected.input.border_w
        && actual.tag == expected.input.tag
        && actual.first_child == expected.expected_first_child
        && actual.next == expected.expected_next
        && actual.rect == expected.expected_rect
}

fn eval_layout_vector(vec: &LayoutVec) -> bool {
    if vec.nodes.is_empty() || vec.nodes.len() > MAX_NODES_PER_CASE {
        return false;
    }

    let mut storage = [Ra8Box::zeroed(); MAX_NODES_PER_CASE];
    let mut tree = Ra8BoxTree::empty();
    let Ok(cap) = u16::try_from(MAX_NODES_PER_CASE) else {
        return false;
    };
    if box_tree_init(Some(&mut tree), Some(&mut storage), cap) != K_RA8_OK {
        return false;
    }

    for node in vec.nodes {
        let idx = box_add(Some(&mut tree), node.parent, Some(&node.input));
        if idx != node.expected_index {
            return false;
        }
    }

    if usize::from(tree.count) != vec.nodes.len() {
        return false;
    }

    if box_layout(Some(&mut tree), vec.root, Some(&vec.frame)) != K_RA8_OK {
        return false;
    }

    for (actual, expected) in storage.iter().zip(vec.nodes.iter()) {
        if !check_node(actual, expected) {
            return false;
        }
    }
    true
}

fn record(out: &mut PairResult, id: &'static str, ok: bool) {
    out.total += 1;
    if ok {
        out.passed += 1;
    } else {
        out.failed += 1;
        if out.failed_id.is_none() {
            out.failed_id = Some(id);
        }
    }
}

/// Evaluate all shared `ra8_box` error and layout vectors through the Zig C ABI.
#[must_use]
pub fn run_all() -> PairResult {
    let mut result = PairResult {
        total: 0,
        passed: 0,
        failed: 0,
        failed_id: None,
    };

    for vec in &ERR_VECTORS {
        record(&mut result, vec.id, eval_err_vector(vec));
    }
    for vec in &LAYOUT_VECTORS {
        record(&mut result, vec.id, eval_layout_vector(vec));
    }
    result
}

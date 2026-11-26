// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Native host parity and execution tests proving `ra8_box_pair_rs` matches
//! `tests/zig_abi_pairs/ra8_box_pair/vectors.json` field-for-field and executes
//! every vector cleanly against `libra8_box.a`.

use core::ffi::c_char;
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicUsize, Ordering};

use ra8_box_pair_rs::{
    CONSTANTS, ERR_VECTORS, ErrOp, LAYOUT_VECTORS, PairResult, Ra8Box, Ra8BoxTree, Ra8UiRect,
    format_banner, run_all,
};

const CANONICAL_JSON: &str =
    include_str!("../../../../../tests/zig_abi_pairs/ra8_box_pair/vectors.json");

static LOG_ERRORS: AtomicUsize = AtomicUsize::new(0);

#[expect(
    unsafe_code,
    reason = "host test stub for ra8_log_emit_error required by libra8_box.a"
)]
#[unsafe(no_mangle)]
pub extern "C" fn ra8_log_emit_error(_tag: *const c_char, _message: *const c_char) {
    LOG_ERRORS.fetch_add(1, Ordering::Relaxed);
}

#[derive(Debug, PartialEq)]
enum Json {
    Bool(bool),
    Int(i64),
    Str(String),
    Array(Vec<Json>),
    Object(BTreeMap<String, Json>),
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn new(src: &'a str) -> Self {
        Self {
            bytes: src.as_bytes(),
            pos: 0,
        }
    }

    fn skip_ws(&mut self) {
        while let Some(&b) = self.bytes.get(self.pos) {
            if matches!(b, b' ' | b'\t' | b'\r' | b'\n') {
                self.pos += 1;
            } else {
                break;
            }
        }
    }

    fn peek(&mut self) -> u8 {
        self.skip_ws();
        self.bytes[self.pos]
    }

    fn expect(&mut self, want: u8) {
        self.skip_ws();
        assert_eq!(
            self.bytes[self.pos], want,
            "unexpected token at {}",
            self.pos
        );
        self.pos += 1;
    }

    fn parse_str(&mut self) -> String {
        self.expect(b'"');
        let start = self.pos;
        while self.bytes[self.pos] != b'"' {
            self.pos += 1;
        }
        let s = std::str::from_utf8(&self.bytes[start..self.pos])
            .expect("UTF-8 JSON string")
            .to_owned();
        self.pos += 1;
        s
    }

    fn parse_int(&mut self) -> i64 {
        self.skip_ws();
        let start = self.pos;
        if self.bytes[self.pos] == b'-' {
            self.pos += 1;
        }
        while let Some(&b) = self.bytes.get(self.pos) {
            if b.is_ascii_digit() {
                self.pos += 1;
            } else {
                break;
            }
        }
        std::str::from_utf8(&self.bytes[start..self.pos])
            .expect("UTF-8 int")
            .parse::<i64>()
            .expect("valid i64")
    }

    fn parse_value(&mut self) -> Json {
        match self.peek() {
            b'"' => Json::Str(self.parse_str()),
            b'{' => self.parse_object(),
            b'[' => self.parse_array(),
            b't' => {
                assert!(self.bytes[self.pos..].starts_with(b"true"));
                self.pos += 4;
                Json::Bool(true)
            }
            b'f' => {
                assert!(self.bytes[self.pos..].starts_with(b"false"));
                self.pos += 5;
                Json::Bool(false)
            }
            b'-' | b'0'..=b'9' => Json::Int(self.parse_int()),
            other => panic!("unexpected JSON byte {other} at {}", self.pos),
        }
    }

    fn parse_array(&mut self) -> Json {
        self.expect(b'[');
        let mut items = Vec::new();
        if self.peek() == b']' {
            self.pos += 1;
            return Json::Array(items);
        }
        loop {
            items.push(self.parse_value());
            match self.peek() {
                b',' => self.pos += 1,
                b']' => {
                    self.pos += 1;
                    break;
                }
                other => panic!("expected ',' or ']', got {other}"),
            }
        }
        Json::Array(items)
    }

    fn parse_object(&mut self) -> Json {
        self.expect(b'{');
        let mut map = BTreeMap::new();
        if self.peek() == b'}' {
            self.pos += 1;
            return Json::Object(map);
        }
        loop {
            let key = self.parse_str();
            self.expect(b':');
            let val = self.parse_value();
            map.insert(key, val);
            match self.peek() {
                b',' => self.pos += 1,
                b'}' => {
                    self.pos += 1;
                    break;
                }
                other => panic!("expected ',' or '}}', got {other}"),
            }
        }
        Json::Object(map)
    }
}

fn field<'a>(obj: &'a BTreeMap<String, Json>, key: &str) -> &'a Json {
    obj.get(key)
        .unwrap_or_else(|| panic!("missing field {key}"))
}

fn as_obj(val: &Json) -> &BTreeMap<String, Json> {
    match val {
        Json::Object(map) => map,
        _ => panic!("expected JSON object"),
    }
}

fn as_arr(val: &Json) -> &[Json] {
    match val {
        Json::Array(items) => items,
        _ => panic!("expected JSON array"),
    }
}

fn as_str(val: &Json) -> &str {
    match val {
        Json::Str(s) => s,
        _ => panic!("expected JSON string"),
    }
}

fn as_bool(val: &Json) -> bool {
    match val {
        Json::Bool(b) => *b,
        _ => panic!("expected JSON bool"),
    }
}

fn as_i64(val: &Json) -> i64 {
    match val {
        Json::Int(i) => *i,
        _ => panic!("expected JSON integer"),
    }
}

fn parse_rect(val: &Json) -> Ra8UiRect {
    let map = as_obj(val);
    Ra8UiRect {
        x: i32::try_from(as_i64(field(map, "x"))).expect("rect.x"),
        y: i32::try_from(as_i64(field(map, "y"))).expect("rect.y"),
        w: i32::try_from(as_i64(field(map, "w"))).expect("rect.w"),
        h: i32::try_from(as_i64(field(map, "h"))).expect("rect.h"),
    }
}

#[test]
fn ffi_abi_layout_matches_c_header() {
    assert_eq!(size_of::<Ra8UiRect>(), 16);
    assert_eq!(size_of::<Ra8Box>(), 48);
    assert!(size_of::<Ra8BoxTree>() >= size_of::<usize>());
    assert_eq!(std::mem::offset_of!(Ra8Box, kind), 0);
    assert_eq!(std::mem::offset_of!(Ra8Box, grid_cols), 1);
    assert_eq!(std::mem::offset_of!(Ra8Box, fixed), 2);
    assert_eq!(std::mem::offset_of!(Ra8Box, flex), 4);
    assert_eq!(std::mem::offset_of!(Ra8Box, fill), 12);
    assert_eq!(std::mem::offset_of!(Ra8Box, first_child), 24);
    assert_eq!(std::mem::offset_of!(Ra8Box, next), 28);
    assert_eq!(std::mem::offset_of!(Ra8Box, rect), 32);
}

#[test]
fn rust_constants_match_canonical_vectors_json() {
    let parsed = Parser::new(CANONICAL_JSON).parse_value();
    let root = as_obj(&parsed);
    assert_eq!(as_i64(field(root, "schema")), 1);
    assert_eq!(as_str(field(root, "library")), "ra8_box");
    assert_eq!(as_str(field(root, "header")), "libs/ra8_box/inc/ra8_box.h");

    let c = as_obj(field(root, "constants"));
    assert_eq!(
        i32::try_from(as_i64(field(c, "k_ra8_box_none"))).unwrap(),
        CONSTANTS.k_ra8_box_none
    );
    assert_eq!(
        i32::try_from(as_i64(field(c, "k_ra8_box_no_colour"))).unwrap(),
        CONSTANTS.k_ra8_box_no_colour
    );
    assert_eq!(
        u8::try_from(as_i64(field(c, "k_ra8_box_stack_v"))).unwrap(),
        CONSTANTS.k_ra8_box_stack_v
    );
    assert_eq!(
        u8::try_from(as_i64(field(c, "k_ra8_box_stack_h"))).unwrap(),
        CONSTANTS.k_ra8_box_stack_h
    );
    assert_eq!(
        u8::try_from(as_i64(field(c, "k_ra8_box_grid"))).unwrap(),
        CONSTANTS.k_ra8_box_grid
    );
    assert_eq!(
        u8::try_from(as_i64(field(c, "k_ra8_box_leaf"))).unwrap(),
        CONSTANTS.k_ra8_box_leaf
    );
    assert_eq!(
        u16::try_from(as_i64(field(c, "k_ra8_ok"))).unwrap(),
        CONSTANTS.k_ra8_ok
    );
    assert_eq!(
        u16::try_from(as_i64(field(c, "k_ra8_err_invalid_arg"))).unwrap(),
        CONSTANTS.k_ra8_err_invalid_arg
    );
    assert_eq!(
        u16::try_from(as_i64(field(c, "k_ra8_err_null_ptr"))).unwrap(),
        CONSTANTS.k_ra8_err_null_ptr
    );
    assert_eq!(
        usize::try_from(as_i64(field(c, "sizeof_ra8_ui_rect_t"))).unwrap(),
        CONSTANTS.sizeof_ra8_ui_rect_t
    );
    assert_eq!(
        usize::try_from(as_i64(field(c, "sizeof_ra8_box_t"))).unwrap(),
        CONSTANTS.sizeof_ra8_box_t
    );
    assert_eq!(
        u16::try_from(as_i64(field(c, "max_nodes_per_case"))).unwrap(),
        CONSTANTS.max_nodes_per_case
    );
}

#[test]
fn rust_error_vectors_match_canonical_vectors_json() {
    let parsed = Parser::new(CANONICAL_JSON).parse_value();
    let root = as_obj(&parsed);
    let json_errs = as_arr(field(root, "error_vectors"));
    assert_eq!(json_errs.len(), ERR_VECTORS.len());
    for (jval, rv) in json_errs.iter().zip(ERR_VECTORS.iter()) {
        let jm = as_obj(jval);
        assert_eq!(as_str(field(jm, "id")), rv.id);
        let op = match as_str(field(jm, "op")) {
            "tree_init" => ErrOp::TreeInit,
            "add" => ErrOp::Add,
            "layout" => ErrOp::Layout,
            other => panic!("unexpected op {other}"),
        };
        assert_eq!(op, rv.op);
        assert_eq!(as_bool(field(jm, "null_tree")), rv.null_tree);
        assert_eq!(as_bool(field(jm, "null_arg")), rv.null_arg);
        assert_eq!(u16::try_from(as_i64(field(jm, "cap"))).unwrap(), rv.cap);
        assert_eq!(
            u16::try_from(as_i64(field(jm, "seed_count"))).unwrap(),
            rv.seed_count
        );
        assert_eq!(
            i16::try_from(as_i64(field(jm, "index_arg"))).unwrap(),
            rv.index_arg
        );
        assert_eq!(
            u16::try_from(as_i64(field(jm, "expected_err"))).unwrap(),
            rv.expected_err
        );
        assert_eq!(
            i16::try_from(as_i64(field(jm, "expected_idx"))).unwrap(),
            rv.expected_idx
        );
    }
}

#[test]
fn rust_layout_vectors_match_canonical_vectors_json() {
    let parsed = Parser::new(CANONICAL_JSON).parse_value();
    let root = as_obj(&parsed);
    let json_layouts = as_arr(field(root, "layout_vectors"));
    assert_eq!(json_layouts.len(), LAYOUT_VECTORS.len());
    for (jval, rv) in json_layouts.iter().zip(LAYOUT_VECTORS.iter()) {
        let jm = as_obj(jval);
        assert_eq!(as_str(field(jm, "id")), rv.id);
        assert_eq!(parse_rect(field(jm, "frame")), rv.frame);
        assert_eq!(i16::try_from(as_i64(field(jm, "root"))).unwrap(), rv.root);
        let jnodes = as_arr(field(jm, "nodes"));
        assert_eq!(jnodes.len(), rv.nodes.len());
        for (jn, rn) in jnodes.iter().zip(rv.nodes.iter()) {
            let nm = as_obj(jn);
            assert_eq!(
                i16::try_from(as_i64(field(nm, "parent"))).unwrap(),
                rn.parent
            );
            assert_eq!(
                u8::try_from(as_i64(field(nm, "kind"))).unwrap(),
                rn.input.kind
            );
            assert_eq!(
                u8::try_from(as_i64(field(nm, "grid_cols"))).unwrap(),
                rn.input.grid_cols
            );
            assert_eq!(
                i16::try_from(as_i64(field(nm, "fixed"))).unwrap(),
                rn.input.fixed
            );
            assert_eq!(
                u16::try_from(as_i64(field(nm, "flex"))).unwrap(),
                rn.input.flex
            );
            assert_eq!(
                i16::try_from(as_i64(field(nm, "pad"))).unwrap(),
                rn.input.pad
            );
            assert_eq!(
                i16::try_from(as_i64(field(nm, "gap"))).unwrap(),
                rn.input.gap
            );
            assert_eq!(
                u32::try_from(as_i64(field(nm, "fill"))).unwrap(),
                rn.input.fill
            );
            assert_eq!(
                u32::try_from(as_i64(field(nm, "border"))).unwrap(),
                rn.input.border
            );
            assert_eq!(
                i16::try_from(as_i64(field(nm, "border_w"))).unwrap(),
                rn.input.border_w
            );
            assert_eq!(
                i16::try_from(as_i64(field(nm, "tag"))).unwrap(),
                rn.input.tag
            );
            assert_eq!(
                i16::try_from(as_i64(field(nm, "expected_index"))).unwrap(),
                rn.expected_index
            );
            assert_eq!(
                i32::try_from(as_i64(field(nm, "expected_first_child"))).unwrap(),
                rn.expected_first_child
            );
            assert_eq!(
                i32::try_from(as_i64(field(nm, "expected_next"))).unwrap(),
                rn.expected_next
            );
            assert_eq!(parse_rect(field(nm, "expected_rect")), rn.expected_rect);
        }
    }
}

#[test]
fn rust_executes_all_shared_vectors_against_libra8_box() {
    let before_logs = LOG_ERRORS.load(Ordering::Relaxed);
    let result = run_all();
    assert_eq!(result.failed, 0, "failed vector: {:?}", result.failed_id);
    assert_eq!(result.total, 18);
    assert_eq!(result.passed, 18);
    assert!(LOG_ERRORS.load(Ordering::Relaxed) > before_logs);
}

#[test]
fn firmware_verdict_banner_matches_hil_contract() {
    let mut buf = [0_u8; 96];
    let pass = format_banner(
        &mut buf,
        &PairResult {
            total: 18,
            passed: 18,
            failed: 0,
            failed_id: None,
        },
    );
    assert_eq!(pass, b"RA8_BOX_PAIR_RS PASS 18/18\r\n");

    let fail = format_banner(
        &mut buf,
        &PairResult {
            total: 18,
            passed: 17,
            failed: 1,
            failed_id: Some("grid_2col_partial_row"),
        },
    );
    assert_eq!(fail, b"RA8_BOX_PAIR_RS FAIL 1/18 grid_2col_partial_row\r\n");
}

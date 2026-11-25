# `ra8_box` Paired ABI Behavior Specification

Single canonical behavior vector contract shared by both the C client and
future Rust client targeting `libs/ra8_box/inc/ra8_box.h`
(`libs/ra8_box/src/ra8_box_abi.zig`).

## Files

- `tests/zig_abi_pairs/ra8_box_pair/vectors.json` -- authoritative inputs, return
  codes, topology expectations, and expected rectangles consumed by both clients.
- `tests/zig_abi_pairs/ra8_box_pair/inc/ra8_box_pair_vectors.h` -- C23 data
  projection derived 1:1 from `vectors.json` for firmware targets without
  dynamic JSON parsing.
- `tests/zig_abi_pairs/ra8_box_pair/tests/verify_vectors_test.zig` -- host parity
  and execution test proving:
  1. `tests/zig_abi_pairs/ra8_box_pair/inc/ra8_box_pair_vectors.h` matches
     `tests/zig_abi_pairs/ra8_box_pair/vectors.json` in every field.
  2. Every vector executes cleanly against `libs/ra8_box`.

## Contract Summary

1. **ABI Size & Alignment Checks**: `sizeof(ra8_ui_rect_t) == 16` and
   `sizeof(ra8_box_t) == 48`.
2. **Error Vectors (`13` cases)**:
   - `ra8_box_tree_init`: null tree/storage (`k_ra8_err_null_ptr`), zero capacity
     (`k_ra8_err_invalid_arg`).
   - `ra8_box_add`: null tree/node, negative/out-of-range parent, capacity
     overflow (`k_ra8_box_none`).
   - `ra8_box_layout`: null tree/frame (`k_ra8_err_null_ptr`), empty tree or
     negative/out-of-range root (`k_ra8_err_invalid_arg`).
3. **Layout Vectors (`5` trees, `22` nodes total)**:
   - `stack_v_padded_fixed_flex`: vertical stack with uniform pad/gap, fixed
     and flex children.
   - `stack_h_weighted_flex`: horizontal stack with non-zero frame origin, fixed
     header, and `1:2` weighted flex split.
   - `grid_2col_partial_row`: 2-column grid with incomplete trailing row and
     fixed-height override cell.
   - `nested_chrome_tree`: parent `stack_v` containing a `stack_h` header and
     3-column `grid` body.
   - `clamp_underflow_zero_floor`: excessive padding clamping dimensions to
     zero and `grid_cols == 0` fallback to 1 column.

## Verification

Run on host with Zig installed:

```sh
zig build --build-file tests/zig_abi_pairs/ra8_box_pair/build.zig test
```

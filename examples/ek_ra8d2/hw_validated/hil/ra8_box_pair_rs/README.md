# `ra8_box_pair_rs`

`#![no_std]` Rust firmware client of the `ra8_box` Zig C ABI
(`libs/ra8_box/inc/ra8_box.h`), forming the Rust half of the first paired
C/Rust example alongside `examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_c/`.

## Behavior Contract

Executes every shared error (`13`) and layout (`5`) vector from
`tests/zig_abi_pairs/ra8_box_pair/vectors.json` directly against `libra8_box.a`
(`ra8_box_tree_init`, `ra8_box_add`, `ra8_box_layout`).

- On full pass: sets `g_ra8_box_pair_rs_pass = 1`, prints
  `RA8_BOX_PAIR_RS PASS <passed>/<total>` on SCI8 (`115200` 8N1) once per
  second, and toggles LED1.
- On failure: prints `RA8_BOX_PAIR_RS FAIL <failed>/<total> <first_id>` on
  SCI8 and halts in `wfi`.

## Building and Verifying

- Cross-build ARM ELF through the root Zig build graph: `just zig::rust-arm`
  (or `just apps::build ra8_box_pair_rs` to stage into `build/`)
  (requires Cargo + `thumbv8m.main-none-eabihf` target stdlib, or
  `-Dra8-box-pair-rs-lib=<dir>` pointing at a prebuilt `libra8_box_pair_rs.a`)
- Host Cargo contract + `vectors.json` parity suite: `cargo test --locked`

Validated on EK-RA8D2 silicon (`RA8_BOX_PAIR_RS PASS 18/18`) alongside
`examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_c/`.

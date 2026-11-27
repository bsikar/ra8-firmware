# `ra8_box_pair_c`

C23 firmware client of the `ra8_box` Zig C ABI (`libs/ra8_box/inc/ra8_box.h`),
forming the C half of the first paired C/Rust example on EK-RA8D2.

## Behavior Contract

Executes all shared error and layout vectors defined in
`tests/zig_abi_pairs/ra8_box_pair/vectors.json` via the freestanding runner
(`tests/zig_abi_pairs/ra8_box_pair/src/ra8_box_pair_runner.c`).

- On full pass: prints `RA8_BOX_PAIR_C PASS <passed>/<total>` on SCI8 (`115200`
  8N1) once per second while toggling LED1.
- On failure: prints `RA8_BOX_PAIR_C FAIL <failed>/<total> <first_id>` on SCI8
  and halts in `wfi`.

## Building and Flashing

- Zig build graph: `just zig::arm`
- CMake build: `just apps::build ra8_box_pair_c`
- Flash (J-Link): `just apps::hardware::flash ra8_box_pair_c`

Validated on EK-RA8D2 silicon (`RA8_BOX_PAIR_C PASS 18/18`) alongside
`examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs/`.

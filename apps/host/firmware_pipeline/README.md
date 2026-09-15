# Firmware Pipeline

`firmware_pipeline` is the reference C23-to-Zig-to-Rust host application. C owns bounded file input, diagnostics, presentation, and cleanup. Zig validates the public ABI request, computes the bytewise XOR, and calls Rust through a second hand-authored C interface. Rust computes counts and an FNV-1a 64-bit change-detection digest. Neither ABI retains a pointer or transfers allocation ownership.

The command accepts exactly one non-empty firmware image of at most 16 MiB. Its output contains Rust-owned count/digest fields and Zig-owned XOR/stage-marker fields, so tests can prove both stages executed. FNV-1a is not a cryptographic authenticity check.

```bash
just apps::host::build firmware_pipeline
just apps::host::run firmware_pipeline args="path/to/firmware.bin"
just quality::devcontainer::lint-c
just quality::devcontainer::lint-rust
just quality::devcontainer::lint-zig
just quality::devcontainer::test-c
just quality::devcontainer::test-rust
just quality::devcontainer::test-zig
```

The public `inc/firmware_pipeline.h` membrane is C-to-Zig. The private `inc/firmware_pipeline_rust.h` membrane is Zig-to-Rust. Both are hand-authored C23 contracts with fixed layout assertions, unchanged-output rules, exact export inventories, and no generated or native-language ABI leakage.

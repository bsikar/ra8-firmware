# Firmware Pipeline

`firmware_pipeline` is one C23-Zig-Rust composition with two real application owners. `firmware_pipeline_rust_main` starts in Rust; `firmware_pipeline_zig_main` starts in Zig. Both call the same C-owned argument and bounded-file support, execute the same Zig public ABI stage, and reach the same Rust provider through the private C ABI. C defines no production `main`.

The command accepts exactly one non-empty firmware image of at most 16 MiB. Its output contains Rust-owned count/digest fields and Zig-owned XOR/stage-marker fields, so tests can prove both stages executed. FNV-1a is not a cryptographic authenticity check.

```bash
just apps::host::build firmware_pipeline
# Run either staged executable from the host-app build directory:
# firmware_pipeline_rust_main path/to/firmware.bin
# firmware_pipeline_zig_main path/to/firmware.bin
just quality::devcontainer::lint-c
just quality::devcontainer::lint-rust
just quality::devcontainer::lint-zig
just quality::devcontainer::test-c
just quality::devcontainer::test-rust
just quality::devcontainer::test-zig
```

The public `inc/firmware_pipeline.h` membrane is caller-to-Zig. The private `inc/firmware_pipeline_rust.h` membrane is Zig-to-Rust. Both are hand-authored C23 contracts with fixed layout assertions, unchanged-output rules, exact export inventories, and no generated or native-language ABI leakage. Command tests require byte-identical output and diagnostics from the Rust-main and Zig-main executables.

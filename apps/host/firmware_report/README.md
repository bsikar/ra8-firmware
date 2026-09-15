# Firmware Report

`firmware_report` is the reference mixed C23-and-Rust host application with a Rust-owned `main`. C owns the deterministic command-line policy; Rust owns process startup, bounded file I/O, diagnostics, presentation, and image analysis. The Rust entry point calls the C parser through its hand-authored C ABI, while the separately tested `inc/firmware_report.h` contract remains the language-independent provider boundary.

The command accepts exactly one firmware image of at most 16 MiB and prints byte count, zero-byte count, erased-flash (`0xff`) count, and an FNV-1a 64-bit change-detection digest. FNV-1a is not a cryptographic authenticity check. Usage, I/O, capacity, ABI, teardown, and output failures return status `2` and write a diagnostic to standard error.

```bash
just apps::host::build firmware_report
just apps::host::run firmware_report args="path/to/firmware.bin"
just quality::devcontainer::lint-c
just quality::devcontainer::test-c
just quality::devcontainer::lint-rust
just quality::devcontainer::test-rust
```

The final executable is produced by Cargo from `rust-main`; CMake supplies the C parser archive. The allocation-free Rust provider permits one live opaque report borrow at a time. `firmware_report_create` borrows the fixed provider slot on success, `firmware_report_query` leaves output unchanged on every error, and `firmware_report_release` ends the borrow and clears the caller's slot. Tests separately cover the C policy, Rust-owned CLI, safe Rust analysis, Rust adapter, linked C ABI, layout, exact export inventory, and release path.

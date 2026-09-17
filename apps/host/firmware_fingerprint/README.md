# Firmware Fingerprint

`firmware_fingerprint` is a deterministic, dependency-free Rust host tool for identifying a firmware image before flashing or comparing build artifacts. It reports the exact byte length and the FNV-1a 64-bit digest of the complete file.

## Input and output

Pass exactly one firmware-image path. The tool writes stable key-value output to standard output:

```text
bytes=5
fnv1a64=a430d84680aabd0b
```

FNV-1a is a fast change detector, not a cryptographic integrity or authenticity check. An empty input is valid. Missing or extra arguments, unreadable files, and output failures return exit status `2`; diagnostics go to standard error and successful output is not emitted after a read failure.

## Build, run, and test

```bash
just apps::host::build firmware_fingerprint
just apps::host::run firmware_fingerprint args="path/to/firmware.bin"
just quality::devcontainer::format-rust
just quality::devcontainer::lint-rust
just quality::devcontainer::test-rust
```

The canonical top-level host CMake build also includes the executable. Its Cargo manifest declares the MIT license and has no third-party dependencies; the registered Rust gate checks both facts against `.rust-test-contract.json`.

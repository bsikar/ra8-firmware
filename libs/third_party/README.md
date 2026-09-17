# Third-Party Libraries

This directory contains platform and middleware vendored dependencies shared
outside a single product domain. Application/content dependencies live under
`apps/shared_libs/third_party/`. Keep both indexes and the SBOM registry updated.

A host tool reusing an application dependency does not make it platform
middleware. Truly tool-exclusive SOUP may instead live in a reserved namespace.
The future path is `tools/<tool>/third_party/<component>`, but only with an SBOM
registry record, upstream manifest, qualification, license inventory entry, and
raw-byte checkout rule. No dependency currently qualifies for that tool-private
shape.

| Library | Description |
|---|---|
| `esp-hosted` | ESP32 Wi-Fi / Bluetooth co-processor firmware and driver |
| `flatbuffers` | Memory-efficient serialization library |
| `gemmlowp` | Low-precision matrix multiplication (used by TFLM) |
| `levelx` | Microsoft Azure RTOS LevelX NAND/NOR flash wear leveling |
| `mbedtls` | ARM Mbed TLS cryptography and SSL/TLS library |
| `netxduo` | Microsoft Azure RTOS NetX Duo TCP/IP IPv4/IPv6 stack |
| `nimble` | Apache NimBLE open-source Bluetooth Low Energy host stack |
| `ruy` | Matrix multiplication library (used by TFLM) |
| `tf-psa-crypto` | Trusted Firmware Platform Security Architecture (PSA) Crypto API |
| `tflite-micro` | TensorFlow Lite for Microcontrollers (ML inferencing) |
| `threadx` | Microsoft Azure RTOS ThreadX real-time operating system |
| `usbx` | Microsoft Azure RTOS USBX host and device stack |

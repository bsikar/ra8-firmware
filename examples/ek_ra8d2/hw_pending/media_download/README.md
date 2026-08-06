# Media Download Example

This example demonstrates how to use the `media_dl` RPC service over Wi-Fi, allowing the RA8D2 to download a book directly to the SD card by communicating with the ESP32-C6 coprocessor.

## Running in the emulator

1. Build the firmware for the emulator target.
2. Run the emulator with the network stub enabled.
3. The emulator output should print progress as it downloads the mocked data.

## Running on the HIL rig

1. Ensure the RA8D2 is connected to the ESP32-C6 and Wi-Fi credentials are provided.
2. An SD card (or XSPI) must be inserted and mounted.
3. Flash the firmware and monitor the UART output for progress logs.

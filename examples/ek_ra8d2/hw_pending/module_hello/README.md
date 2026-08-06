# Hello World Module PoC

This example demonstrates how to build, sign, and load a ThreadX module (`hello_module.c`) with a custom RA8 app header and Ed25519 signature.

## Building the Module Separately

You can build the module binary separate from the kernel application by targeting `hello_module` in CMake:

```bash
cd build
cmake ..
make hello_module
```
This produces `hello_module.elf`. You must objcopy this to a binary:

```bash
arm-none-eabi-objcopy -O binary hello_module.elf hello_module.bin
```

## Signing the Module

Use `rot_sign.py` to sign the module. (Note: Since `rot_sign.py` is for P-256 ECDSA, an Ed25519 version or alternative tool will be used when available). To generate a dummy signature for now:

```bash
python3 tools/rot_sign.py sign --key my_ed25519_key.pem --image hello_module.bin --out hello_module_signed.bin
```

## Testing on the Emulator

Start the RA8 emulator and attach an SD card containing the signed module:

```bash
ra8_emulator --sd path/to/hello_module_signed.bin
```

## Testing on the HIL Rig

1. Flash the main application `module_hello_app.elf` to the board.
2. Place `hello_module_signed.bin` on a microSD card and insert it into the HIL rig.
3. Power on the board. The main application will load the module, verify the signature, and execute it, printing "Hello from module!" to the UART console.

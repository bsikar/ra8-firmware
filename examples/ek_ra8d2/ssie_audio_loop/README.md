# ssie_audio_loop

SSIE I2S internal-loopback audio integrity demo for the bare EK-RA8D2
EVM.

Brings up SSIE0 in controller / I2S mode (16-bit data, 32-bit system word,
AUDIO_MCK / 32 bit clock divider), pushes a 16-sample sine pattern
through the TX FIFO, then logs `ssie: loop ok` over the J-Link OB CDC
console (SCI8 @ 115200 8N1).

Build / flash:

```
make ssie_audio_loop
make -C examples/ek_ra8d2/ssie_audio_loop flash
```

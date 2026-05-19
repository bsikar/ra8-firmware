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

## HIL plan

**HIL-able now -- proposed mode: `uart_scrape`.** The loopback is
internal to SSIE0 (TX FIFO -> RX FIFO without leaving the chip), so no
external audio codec or speaker is required despite the demo name.
The firmware prints `ssie: loop ok` on the J-Link OB CDC port after a
successful 16-sample sine round-trip. A tight `uart_scrape` config:

```
HIL_MODE=uart_scrape
HIL_EXPECT="ssie: loop ok"
HIL_EXPECT_NEGATIVE="ssie: loop FAIL|HardFault"
HIL_TIMEOUT_S=10
```

(Verify the FAIL banner text matches actual firmware before
committing the hil.conf.) For an actual audio CODEC demo with an
external speaker / mic, that would need real hardware and stays
out-of-scope.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the loopback OK path.

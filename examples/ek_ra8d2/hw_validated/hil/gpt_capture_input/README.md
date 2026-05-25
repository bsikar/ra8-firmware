# gpt_capture_input

GPT input-capture-style demo for the bare EK-RA8D2 EVM. Configures a
GPT channel for input-capture on a pin that the user pulses via SW1,
then measures the period between presses in GPT counter ticks. LED1
toggles on every captured event.

Build / flash:

```
make gpt_capture_input
make -C examples/ek_ra8d2/gpt_capture_input flash
```

## HIL plan

**Requires physical stim -- needs external pulses on the capture pin.**
On a bare EVM with no Pi GPIO wired to the capture input pin, the
GPT counter never sees an edge and the demo produces no observable
behaviour except LED1 staying off. No UART output is emitted, so
`uart_scrape` is not an option.

To make this HIL-able: wire a Pi GPIO to the capture pin and add a
`hil_gpio_stim` helper that produces a known pulse train. Instrument
main.c with a `g_gpt_capture_count` / `g_gpt_capture_period_ticks`
pair, then `jlink_memprobe` could assert the count advances and the
measured period is within tolerance of the stim frequency. Until
that wiring exists this stays manual.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the input-capture path.

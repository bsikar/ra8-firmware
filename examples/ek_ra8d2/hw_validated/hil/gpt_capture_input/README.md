# gpt_capture_input

Configures a GPT channel for input capture on a pin pulsed by SW1 and measures
the period between presses in counter ticks; LED1 toggles on every captured
event.

On a bare EVM nothing drives the capture pin, so the counter never sees an edge
and the app produces no observable behaviour -- and there is no UART output
either. The automated gate therefore only probes a tick counter over SWD to
confirm the GPT counter is free-running. Proving the capture path needs a known
pulse train on the capture pin, from a human pressing SW1 or from a stimulus
GPIO wired to it.

# timer_capture_demo

Runs GPT0 free-running and reports the counter delta across a fixed software
delay, once a second, with LED1 as a heartbeat.

The "capture" is a double snapshot around that delay, not the silicon's
GTIOC-edge input-capture mode: a stock EK-RA8D2 has no external GTIOC source
wired without a shield. A delta of zero means the counter never advanced, which
is a real failure mode seen on this chip -- which is why the gate rejects a
zero delta rather than just matching the banner prefix.

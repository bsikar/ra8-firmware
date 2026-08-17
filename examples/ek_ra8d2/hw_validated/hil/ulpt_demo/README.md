# ulpt_demo

Brings up ULPT0 at roughly 1 Hz off the LOCO and treats each underflow as a wake
event: log a line, stop the timer to clear `ULPTCR.TUNDF`, re-arm with the same
period.

The underflow is polled through `ra8_ulpt_get_status` rather than taken as an
interrupt. That is deliberate -- polling needs no NVIC, so the host unit tests
drive the identical register sequence that runs on silicon.

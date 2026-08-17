# c6_wifi_link

Takes the ESP32-C6's Wi-Fi station up through `libs/ra8_c6link` -- the single
integration boundary application code is meant to use -- and reads the station's
MAC address back. It is the first application on this board that speaks
esp-hosted without building the protocol itself. No network is joined; that is
`c6_wifi_join`.

## Why it has to run on a bench at all

Host tests drive every layer of the facade against a co-processor model that
decodes what the host transmits with the same generated codec the C6 runs, so
they prove this firmware *encodes* the Wi-Fi init request correctly. They cannot
prove the co-processor *accepts* it: the scalars that request carries -- a magic
word, buffer counts, aggregation flags, a queue count -- are validated by the far
side's own `esp_wifi_init()`, against limits belonging to the co-processor's
build.

So a FAIL prints the co-processor's `esp_err_t` verbatim next to the RPC id that
produced it. An invalid-argument code against the init request means the
transmitted configuration, not the link; the values and the reasoning behind
each live with the request structure in `libs/ra8_c6link`.

## Readiness is asked for, never waited for

Readiness is deliberately established with a version round-trip rather than the
co-processor's boot event. That event fires once, when the *co-processor* boots,
and the C6 has its own supply -- so resetting this board does not reboot it. An
earlier revision waited for the event and therefore passed exactly once, on a
freshly-flashed co-processor. The identity that comes back is checked against
the vendored host driver's own version, which makes it a host/co-processor
version lock rather than a literal written down twice.

## Triage

If `c6_spi_probe` reports every wire stuck high, the MCU cannot pull those lines
low at all and the fault is the switch bank or the harness -- not the
co-processor, which boots happily on its own USB and answers on its own console.
In that state every app in this tier reads `0xff` off CIPO and fails
identically, so a red here means "look at the bench", not "look at the
firmware".

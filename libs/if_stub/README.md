# if_stub

The proof binding of epic RA8FW-618 slice 5: the `if_<chip>` set a second
target supplies in place of the RA8 adapters, proving a portable library needs
no edit when the chip changes. One member per neutral port, each a complete
binding in the shape its RA8 counterpart set:

| Member | Port | RA8 counterpart |
|---|---|---|
| `clock` | `fw_if_clock` | `if_ra8_cgc` |
| `rng` | `fw_if_rng` | the RA8 TRNG driver's port face |
| `gpio` | `fw_if_gpio` | the RA8 GPIO adapter's port face |
| `uart` | `fw_if_uart` | the RA8 SCI adapter's port face |

`clock` carries a deliberately tiny, deliberately arbitrary profile (the core,
UART 0 and SPI 0 at fixed rates) so the port's rate, floor and gating intents
run end to end through the facade. Gating succeeds and does nothing; absent
modules answer `k_ra8_err_not_found`; a real port replaces the member by
member, and nothing above `libs/if` changes.

`rng` carries a deterministic profile, not entropy: fill N hands back byte i
as (i + N) mod 256, so consecutive fills differ and the port's fill and ready
intents run end to end. The header names this in its own docs: the bytes are
never key material, and a real chip port replaces the member.

`gpio` carries a four-pin board in dense numbering: an output pin's level is
what a write left there, an input pin samples the board's tied level (low on
every stub pin), a write to an input is refused as the port's own contract
says, and every pin past index 3 answers not_found.

This is not a port of any chip. It is named `if_stub`, not after a target, so
nobody mistakes it for the start of an `if_esp32c6` or `if_ti` set; the first
real chip port retires it.

`uart` carries two lines in dense board numbering, a console and a modem, each
coming up at the stub board's firmware default of 115200 8N1. A write is
captured into a per-line ring in order, a read drains a per-line receive queue
a test primes (a quiet line reads zero, not an error), a zero baud or an
out-of-range field is refused, and every line past index 1 answers not_found.
The receive side is a test seam, not product surface: a real chip port
replaces the member, and nothing above `libs/if` changes.

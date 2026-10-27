# if_stub

The proof binding of epic RA8FW-618 slice 5: the `if_<chip>` set a second
target supplies in place of the RA8 adapters, proving a portable library needs
no edit when the chip changes. One member per neutral port, each a complete
binding in the shape its RA8 counterpart set:

| Member | Port | RA8 counterpart |
|---|---|---|
| `clock` | `fw_if_clock` | `if_ra8_cgc` |
| `rng` | `fw_if_rng` | the RA8 TRNG driver's port face |

`clock` carries a deliberately tiny, deliberately arbitrary profile (the core,
UART 0 and SPI 0 at fixed rates) so the port's rate, floor and gating intents
run end to end through the facade. Gating succeeds and does nothing; absent
modules answer `k_ra8_err_not_found`; a real port replaces the member by
member, and nothing above `libs/if` changes.

`rng` carries a deterministic profile, not entropy: fill N hands back byte i
as (i + N) mod 256, so consecutive fills differ and the port's fill and ready
intents run end to end. The header names this in its own docs: the bytes are
never key material, and a real chip port replaces the member.

This is not a port of any chip. It is named `if_stub`, not after a target, so
nobody mistakes it for the start of an `if_esp32c6` or `if_ti` set; the first
real chip port retires it.

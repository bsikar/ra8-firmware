# ra8_c6link

The one boundary between this firmware and the ESP32-C6 co-processor.
Application code reaches the radio here and nowhere else: above this library
there is no protobuf, no TLV envelope, no payload header, no checksum and no
SPI, only a link you open, poll, send Ethernet frames on, and receive events
from. The transport underneath is bound by the caller
(`ra8_esp_hosted_c6link_bind()` in `port/esp-hosted/`), so which wire the
co-processor sits on stays a composition decision.

## Why the API is RA8-native rather than ESP-IDF-shaped

An early design proposed reproducing
the ESP-IDF `wifi_*_t` / `esp_netif_*` types here on the grounds that the C6
decodes their byte layouts. It does not, and the bench settled it: the wire
types are protobuf messages with named fields -- a station config carries its
SSID and passphrase as length-delimited binary -- and upstream's own host
converts into them field by field. Padding, field order and struct size on this
side never reach the co-processor at all.

Copying those types would therefore have bought ESP-IDF *source* compatibility
and contributed nothing to wire correctness. This library defines the handful
of small types a caller genuinely needs instead: an SSID and a passphrase, a
MAC address, an AP record, a few event kinds. A station join is a handful of
RPC ids out of the several hundred the protocol defines, and every one of them
was already present in the vendored codec.

## No heap, and no borrowed one either

The protobuf unpacker allocates. This firmware has none -- `_sbrk` is trapped
as a fatal error -- so the codec is handed a bump allocator over a
caller-supplied array, emptied after every message. The peak requirement is one
message rather than one run, which is what keeps the whole control plane inside
NASA Power of 10 Rule 3 without borrowing an RTOS pool. `free` is not a no-op
here: it rolls the bump offset back when the released block is the newest one,
which is exactly the shape of protobuf-c's own unwind on a failed decode.

## Two facts about this co-processor the library is built on

- **The usable boot announcement is `Event_ESPInit`**, surfaced as
  `k_ra8_c6link_event_boot`.
- **`ESP_PRIV_IF`'s `ESP_PRIV_EVENT_INIT` is unusable on this build** and
  nothing here depends on it: it is transmitted with a non-zero `if_num` but
  checksummed as if that nibble were zero, so every conformant host drops it
  (RA8FW-276). Frames on that
  interface are counted as unrouted and ignored.

Also: the co-processor's idle filler frame legitimately carries `offset = 0`.
Judging it by data-frame rules is what made a healthy link report a failure
once already, so the idle frame gets its own verdict rather than being called
malformed.

## Tested without hardware

The modelled ESP32-C6 under `tests/` is not a recorded byte stream: it
**decodes** what the host transmits with the same generated codec the real part
runs and **synthesises** the answer the co-processor would send. A request this
host encodes wrongly fails to decode there; an answer it decodes wrongly fails
an assertion. It also models the property that trips up full-duplex hosts --
the co-processor latches its transmit buffer before it sees the host's, so an
answer never rides the same transaction as its question.

One thing a host test cannot settle, which is why `c6_wifi_link` exists on the
bench: the WiFi init request carries a block of scalars that the far side's own
`esp_wifi_init()` validates against limits belonging to the co-processor's
build. This side can be proven to encode them; only the C6 can say whether it
accepts them.

## Recording bench traffic

The model answers what this host sends, so it can only be as right as the
traffic it was written from. To check it against the real part, a bench image
wraps its transport with `ra8_c6link_capture_bind` (`ra8_c6link_capture.h`)
and passes the result to `ra8_c6link_open`. Every transaction is then printed
on the debug console, one line per frame:

```
c6cap 41 hs 1
c6cap 41 tx 0200100c...
c6cap 41 rx 01000c00...
```

`tx` and `rx` are the frames clocked out and in, with trailing zero bytes
dropped to keep the console fast. `hs` is a HANDSHAKE level change seen before
that transaction. DATA_READY isn't recorded, because it isn't part of the
transport (see `ra8_c6link_transport.h`). Images that never bind the wrapper
are unchanged.

`scripts/c6cap_to_bin.py` turns a saved console log into a fixture: for each
transaction in order, the 1600-byte frame out, then the 1600-byte frame in,
padded back to full size. `--first` and `--last` cut out one exchange (for
example the Wi-Fi scan) from a longer run. The script refuses a log with a
gap in the sequence numbers, a duplicate line or a frame over 1600 bytes,
because a fixture built from a damaged log would not be the traffic that
crossed the wire.

Before committing a scan fixture, check that the AP records hold only the
bench's own soft-AP. A scan reports every network in range, and a
neighbour's SSID doesn't belong in the repository.

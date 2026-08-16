# ra8_c6link -- the one boundary between this firmware and the ESP32-C6

Application code reaches the co-processor here and nowhere else. Above this
library there is no protobuf, no TLV envelope, no payload header, no checksum
and no SPI: there is a link you open, poll, send Ethernet frames on, and receive
events from.

```c
ra8_c6link_cfg_t cfg = {};
(void)ra8_esp_hosted_c6link_bind(&cfg.transport);   /* port/esp-hosted/ */
cfg.arena       = arena;                            /* caller-owned, >= 2 KiB */
cfg.arena_bytes = (uint32_t)sizeof arena;
cfg.event_cb    = on_event;

(void)ra8_c6link_open(&link, &cfg);
ra8_c6link_fw_version_t fw = {};
(void)ra8_c6link_await_ready(&link, k_ra8_c6link_announce_transfers, &fw);
(void)ra8_c6link_wifi_start(&link);
(void)ra8_c6link_wifi_join(&link, &sta);
```

## Why the API looks like this

`#490` was opened to decide between reproducing ~43 ESP-IDF `wifi_*_t` /
`esp_netif_*` types "whose byte layouts the C6 decodes", or forking a narrower
RA8-native API. The premise turned out to be wrong, and the bench proved it:

- the generated codec mentions none of those types (`grep -c 'wifi_config_t\|esp_netif'`
  over `esp_hosted_rpc.pb-c.{h,c}` returns `0`);
- the wire types are **protobuf messages with named fields** -- `WifiStaConfig`
  carries `ssid` and `password` as `ProtobufCBinaryData` -- and upstream's own
  host converts to them **field by field**;
- so padding, field order and struct size on this side never reach the
  co-processor at all.

Reproducing those types would therefore have bought ESP-IDF *source*
compatibility and contributed nothing to wire correctness. This library defines
the handful of small types a caller genuinely needs instead: an SSID and a
passphrase, a MAC address, an AP record, four event kinds.

A station join is **eleven** RPC ids out of the several hundred the protocol
defines, and every one of them was already generated in the vendored codec.

## Layers

| File | Owns |
|---|---|
| `ra8_c6link_transport.h` | the three-row hardware seam -- transfer, handshake, delay |
| `src/ra8_c6link_frame.c` | the twelve-byte payload header and its checksum |
| `src/ra8_c6link_tlv.c` | the serial endpoint's two-tag envelope |
| `src/ra8_c6link_arena.c` | the fixed decode arena the protobuf codec allocates from |
| `src/ra8_c6link_rpc.c` | request, response, UID correlation, event decode |
| `src/ra8_c6link_pump.c` | the transaction loop and frame routing |
| `src/ra8_c6link.c` | handle lifecycle, dispatch, the identity request, the data plane |
| `src/ra8_c6link_wifi.c` | radio lifecycle: init, mode, start, stop |
| `src/ra8_c6link_wifi_sta.c` | credentials, association, MAC address, AP record |

The media-download extension has one checked-in schema at
`proto/ra8_media_download.proto`. Its generated protobuf-c codec is reproduced
with `scripts/gen/gen_ra8_media_proto.sh`; `--check` fails when the committed C
and header drift from the schema. The script intentionally requires the exact
generator pair used for the committed output (protobuf-c 1.5.2 and libprotoc
35.1) so regeneration is deterministic rather than dependent on the developer's
installed compiler.

Protocol version 2 carries one `ra8_mdl_format_t` value in `StartRequest` and
echoes it in `Accepted`. `loose` means an untyped source body; named artifacts
such as RABOOK must be supplied in that exact representation and pass the
RA8-owned validator before the storage transaction can publish them.

## No heap, and no borrowed one either

`rpc__unpack()` allocates. This firmware has none -- `_sbrk` is a strong symbol
that reports a fatal error -- so the codec is handed a bump allocator over a
**caller-supplied array**, emptied after every message. The peak requirement is
one message rather than one run, which is what keeps the whole control plane
inside NASA Power of 10 Rule 3 without borrowing an RTOS pool. `free` is not a
no-op: it rolls the bump offset back when the released block is the newest one,
which is exactly the shape of protobuf-c's own unwind on a failed decode.

## Two facts about this co-processor the library is built on

- **The usable boot announcement is `Event_ESPInit`**, surfaced as
  `k_ra8_c6link_event_boot`.
- **`ESP_PRIV_IF`'s `ESP_PRIV_EVENT_INIT` is unusable on this build** and
  nothing here depends on it: it is transmitted with a non-zero `if_num` but
  checksummed as if that nibble were zero, so every conformant host drops it
  ([#529](https://github.com/bsikar/ra8-firmware/issues/529)). Frames on that
  interface are counted as `unrouted` and ignored.

Also: the co-processor's idle filler frame legitimately carries `offset = 0`.
Judging it by data-frame rules is what made a healthy link report a failure
once already, so `k_ra8_c6link_frame_idle` is its own verdict.

## Tested without hardware

`tests/mocks/ra8_c6_model.c` is a modelled ESP32-C6, and it is not a recorded
byte stream: it **decodes** what the host transmits with the same generated
codec the C6 runs and **synthesises** the answer the co-processor would send. A
request this host encodes wrongly fails to decode there; an answer it decodes
wrongly fails its assertion. It also models the property that trips up
full-duplex hosts -- the co-processor latches its transmit buffer before it sees
the host's, so an answer never rides the same transaction as its question.

- `tests/test_ra8_c6link_wire.c` -- the pure layers on their own, including the
  octets of the header and the envelope compared against protocol literals
  rather than against the code that wrote them.
- `tests/test_ra8_c6link.c` -- the whole facade: identity round trip, UID and
  message-id correlation, the station start sequence, credentials arriving
  intact, MAC and AP record decode, announcements, the Ethernet data plane,
  transport faults and undecodable frames.

## What silicon adds

`examples/ek_ra8d2/hw_validated/c6/c6_wifi_link` drives this library against the
real co-processor (`make hil-c6 APP=c6_wifi_link`). It exists because of one
thing a host test cannot settle: `Req_WifiInit` carries twenty scalars that the
far side's own `esp_wifi_init()` validates against limits belonging to the
co-processor's build. This side can be proven to encode them; only the C6 can
say whether it accepts them.

# c6_fw_version -- an esp-hosted RPC round-trip, with the answer checked

The first application to prove the esp-hosted **protocol** rather than the
wire. `c6_spi_probe` established the physical link and `c6_hosted_init`
established that the port can clock a framed transaction; both stop at "bytes
moved, and their shape is sane". This one sends a request the co-processor must
parse, receives the response it generates, decodes it with the vendored
protobuf codec, and then checks what is in it.

```sh
make hil-c6 APP=c6_fw_version
```

Bench requirements are the tier's: SW4 1=OFF 2=OFF 3=ON 4=OFF, the harness on
J26, the C6 on its own USB. See [`../README.md`](../README.md).

## What it asserts

Four things, all on the response's contents:

| Field | Expected | Where the expectation comes from |
|---|---|---|
| `uid` | the request's UID | This is what makes it a round trip and not a coincidence: the co-processor echoes the UID back. |
| `resp` | `0` | The co-processor's own result code for the call. |
| `major.minor.patch` | `2.12.11` | `ESP_HOSTED_VERSION_{MAJOR,MINOR,PATCH}_1` from the **vendored host driver's** `esp_hosted_host_fw_ver.h`. Not a literal. |
| `chip_id` | `0x0D` | `ESP_PRIV_FIRMWARE_CHIP_ESP32C6` from the vendored `transport_drv.h`. Not a literal. |

The version expectation is deliberately the host driver's own version rather
than a number written down here. The co-processor image and the vendored host
come from one pinned upstream commit, so they must agree exactly -- which makes
this test the **host / co-processor version lock** that #316 exists to enforce.
Bump the vendor pin without reflashing the C6, or reflash the C6 without
bumping the pin, and this app goes red on purpose instead of the mismatch
surfacing later as unexplained RPC timeouts.

`idf_target` is printed but not asserted, through
`c6_fwver_put_text()`, which truncates and strips non-printable bytes: it is a
string the co-processor chose, and a protocol field that can reprogram a
terminal is not evidence.

## Observed output (2026-07-28, first silicon run)

```
c6_fwver: EK-RA8D2 <-> ESP32-C6 esp-hosted RPC round-trip
c6_fwver: cpuclk0_hz=1000000000 pclka_hz=125000000
c6_fwver: spi sci=2 sck_hz=5000000 frame_bytes=1600 max_payload=1588
c6_fwver: entering ThreadX; port init runs from tx_application_define
c6_fwver: port_init=ok
c6_fwver: host caps build=ok bytes=17
c6_fwver: dropped bad-checksum if_type=5 if_num=8 flags=0x00 len=29 offset=12 seq=0 pkt_type=0x33 csum=0x026d calc=0x02ed raw=85001d000c006d0200000033
c6_fwver:   ^ explained: the co-processor checksummed this frame with if_num=0 and then transmitted a non-zero if_num. A conformant host drops it -- upstream's own process_spi_rx_buf() would too.
c6_fwver: frame if_type=3 if_num=0 len=22
c6_fwver: rpc in type=3 id=769 uid=0 proto_bytes=10
c6_fwver: pump caps xfers=8 frames=1 idle=6 badcsum=1 malformed=0 hs_timeouts=0 ifnum_defect=1 bus_error=n stopped=budget
c6_fwver: request build=ok bytes=22
c6_fwver: frame if_type=3 if_num=0 len=72
c6_fwver: rpc in type=2 id=606 uid=1 proto_bytes=60
c6_fwver: pump rpc xfers=3 frames=1 idle=2 badcsum=0 malformed=0 hs_timeouts=0 ifnum_defect=0 bus_error=n stopped=sink
c6_fwver: expect uid=1 resp=0 fw=2.12.11 (the vendored host driver's own version) chip_id=0x0d
c6_fwver: rsp uid=1 resp=0 fw=2.12.11 chip_id=0x0d idf_target=esp32c6
c6_fwver: PASS esp-hosted RPC round-trip, co-processor firmware 2.12.11
```

Reading it:

- The request is 22 bytes on the wire: a 10-byte packed `Rpc` protobuf inside a
  12-byte TLV envelope. The response is 72: a 60-byte protobuf in the same
  envelope.
- The round trip took **three transactions** -- the one carrying the request,
  then two more for the co-processor to hand the answer back in. The pump stops
  the instant the RPC layer accepts it.
- `badcsum=0 malformed=0 hs_timeouts=0 bus_error=n` on the RPC pump: the link is
  clean at 5 MHz, five times the rate the original probe qualified.
- `id=769` on the caps pump is `RPC_ID__Event_ESPInit` -- the co-processor's
  unsolicited "I am up" announcement, which arrives as an **RPC event on
  `ESP_SERIAL_IF`**, decoded by the same codec. It only appears when the C6 has
  been power-cycled since its last drain.

## The `ESP_PRIV_IF` INIT event is unusable on this co-processor build

The `dropped bad-checksum` line above is a real, reproducible co-processor-side
defect, and it is worth understanding because it constrains how the facade
above this layer (#490) may be built.

The C6's bootup `ESP_PRIV_IF` INIT event -- the frame that carries its
capabilities, chip id and firmware version as TLVs -- arrives with header byte
zero `0x85`, i.e. `if_type = 5` (`ESP_PRIV_IF`) and `if_num = 8`. Its checksum,
however, was computed over that byte as `0x05`. The shortfall is exactly
`if_num << 4 = 0x80`, and the arithmetic closes precisely:

```
raw   = 85 00 1d 00 0c 00 6d 02 00 00 00 33
stated checksum = 0x026D
recomputed      = 0x02ED      (delta = 0x80)
```

Every other frame the co-processor sends carries `if_num = 0` and verifies.
This is therefore not a receive-path fault on the RA8 side: a corrupted MSB
would have broken the `ESP_SERIAL_IF` frames too (`if_type = 3`, byte zero
`0x03`), and it did not.

Any conformant host drops the frame -- **including upstream's own
`process_spi_rx_buf()`**, which computes exactly this checksum. This
application diagnoses the condition by name and counts it as `ifnum_defect`,
but does **not** accept the frame: the bytes on the wire do not match the
integrity check that accompanies them, and decoding them anyway would mean
trusting an unverified header.

Consequences, tracked as #529:

- The co-processor's capability / chip-id / firmware-version announcement over
  `ESP_PRIV_IF` cannot be relied on. Nothing above this layer should be built
  on `process_init_event()`'s TLVs.
- Nothing is lost. `RPC_ID__Event_ESPInit` gives a boot announcement that
  verifies, and `Req_GetCoprocessorFwVersion` returns the version, the chip id
  and the IDF target on demand.

`c6_fwver_priv_consume()` -- the TLV decoder for that event -- is therefore
present, correct and **unexercised on this bench**. It is kept rather than
deleted because it is a real implementation of a real protocol path that will
run the moment the co-processor firmware is fixed; the host-capabilities half
of the same module (`c6_fwver_priv_host_caps()`) transmits on every run.

## How the layers are wired, and what is first-party

| Layer | Where it comes from |
|---|---|
| Protobuf message + codec | Vendored: `common/proto/esp_hosted_rpc.pb-c.c` and the `protobuf-c` runtime, both already compiled by `cmake/esp_hosted.cmake`. The request is **not** hand-encoded -- hand-encoding would only prove this file and the co-processor agree, which is far weaker than the generated codec and the co-processor agreeing. |
| TLV envelope | First-party, `src/c6_fwver_rpc.c`, mirroring `compose_tlv()` / `parse_tlv()` in the vendored `serial_if.c`. That file is not compiled here because its send path expands `HOSTED_CALLOC`, whose failure arm is a `goto` (NASA Rule 1). The envelope is six bytes of framing; restating it beats importing a rule violation. |
| Payload header, checksum, transaction | First-party, `src/c6_fwver_link.c`, mirroring `get_next_tx_buffer()` and `process_spi_rx_buf()` in the vendored `spi_drv.c`. The vendored transaction task lives in `transport_drv.c`, which is written against ESP-IDF's Wi-Fi API and is deliberately not built here. |
| SPI, GPIO, ThreadX objects | `port/esp-hosted/`, reached only through the vendored `g_h.funcs` vtable -- so the pump sees exactly what the vendored driver would see. |

### Allocation

`rpc__unpack()` needs an allocator and this image has no heap (`_sbrk` is a
strong symbol that reports a fatal error). It is given a `ProtobufCAllocator`
backed by the esp-hosted port's fixed ThreadX byte pool -- the same storage the
vendored transport uses, carved once at init -- which is what keeps the decode
inside NASA Power of 10 Rule 3. Every unpacked message is freed on every path.

## Files

| File | Purpose |
|---|---|
| `c6_fwver.h` | Shared contract: pacing, buffer bounds, formatter widths, the frame union, module entry points |
| `main.c` | Bring-up, ThreadX entry, the two exchange phases, verdict, heartbeat |
| `src/c6_fwver_console.c` | Bounded console formatters (no newlib `printf`) and the two report lines |
| `src/c6_fwver_link.c` | The transaction pump: frame build, HANDSHAKE wait, transfer, receive classification |
| `src/c6_fwver_priv.c` | `ESP_PRIV_IF`: host-capabilities frame out, INIT-event TLV decode in |
| `src/c6_fwver_rpc.c` | `ESP_SERIAL_IF`: protobuf request, TLV envelope, response decode, verdict |

## See also

- [`../README.md`](../README.md) -- the tier: bench setup, DIP switches, the
  triage ladder.
- [`../c6_hosted_init/README.md`](../c6_hosted_init/README.md) -- the layer
  below: the port on silicon.
- `docs/SOUP/esp-hosted-host.md` -- which vendored translation units are
  compiled, and why the rest are not.

USB CDC Echo Throughput Benchmark Results
==========================================

Hardware:     Renesas EK-RA8D2 (Cortex-M85 @ 1 GHz)
Host:         Raspberry Pi 5 (Linux 6.8, cdc_acm kernel driver)
Method:       chunked round-trip echo (host writes chunk, waits for echo,
              verifies, repeats). Each chunk gets one bulk-OUT URB and one
              bulk-IN URB; no pipelining. Zero chunk integrity failures.

USBHS (J7, 480 Mbps line rate):
  Chunk=64    :   57.6 KB/s  ( 115.1 KB/s aggregate)
  Chunk=128   :  114.9 KB/s  ( 229.7 KB/s aggregate)
  Chunk=256   :  229.9 KB/s  ( 459.9 KB/s aggregate)
  Chunk=512   :  460.3 KB/s  ( 920.7 KB/s aggregate)  <-- MPS, max
  Sustained 64KB at 512B chunks: 460.3 KB/s  (920.7 KB/s aggregate, ~7.4 Mbps)

USBFS (J11, 12 Mbps line rate):
  Chunk=32    :   28.8 KB/s  (  57.6 KB/s aggregate)
  Chunk=64    :   57.5 KB/s  ( 115.0 KB/s aggregate)  <-- MPS, max
  Sustained 4KB at 64B chunks: 57.5 KB/s  (115.0 KB/s aggregate, ~0.92 Mbps)

Both controllers' throughput scales linearly with chunk size up to bulk-MPS.
Above MPS, each "chunk" is split into multiple packets and the per-chunk
round-trip latency dominates.

Correctness:
  USBHS  :  64/64 lengths 1..64 echo perfectly, 50/50 random 1..255 B
            payloads echo perfectly, every chunk in 64 KB round-trip
            verifies bit-exact.
  USBFS  :  64/64 lengths 1..64 echo perfectly, 50/50 random payloads
            echo perfectly, 4 KB round-trip verifies bit-exact.

PPPS re-enumeration (uhubctl-driven hub-port data toggle):
  USBHS  :  reliable -- 5/5 cycles
  USBFS  :  not reliable on Linux; the device-side D+ pull-up does not
            resync after a hub-side data toggle. Use Tapo hard power
            cycle for re-enumeration recovery instead.

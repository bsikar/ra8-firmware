# rpc_ring_dualcore

Bidirectional dual-core (Cortex-M85 and Cortex-M33) `ra8_rpc` verification over the shared-SRAM ring transport (`0x2210_0000`) and RA8D2 IPC FIFO doorbells (`0x4002_0000`).

- CPU0 (M85) boots with I/D-cache enabled (`cache_mpu`) and verifies the Normal Non-cacheable MPU region covering shared SRAM before formatting all four rings via `Ring.writeHeader()`, then releases CPU1.
- Pair 0 (`IPC1 FIFO10` / `IPC0 FIFO00`) carries CPU0 client calls to CPU1 and CPU1 events back to CPU0.
- Pair 1 (`IPC0 FIFO01` / `IPC1 FIFO11`) carries CPU1 client calls to CPU0 and CPU0 events back to CPU1.
- After Round 1, CPU0 faults a ring header (`BadMessage`), holds CPU1 while reformatting both ring pairs (`Ring.writeHeader`), resets both `RingTransport`s (`resetAfterReformat`), and completes Round 2.

Built purely in Zig via `zig build arm` (`zig-out/arm/rpc_ring_dualcore.elf` and `rpc_ring_dualcore_cpu1.elf`).

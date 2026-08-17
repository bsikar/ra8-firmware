# dma_coherency_hil

Proves the clean-before / invalidate-after DMA cache-maintenance pattern keeps a
DMAC0 mem-to-mem copy coherent with the M85 L1 D-cache **turned on**. Built with
`RA8_BOOT_ENABLE_CACHE_MPU`, and both buffers sit in the cacheable M85-private
SRAM region, so the hazard is real on silicon.

With the D-cache on, a DMAC copy is no longer automatically coherent in either
direction:

- Freshly written source bytes are dirty in cache, not in SRAM. The DMAC reads
  SRAM, so it copies stale data unless the source is **cleaned** first.
- The DMAC writes the destination straight to SRAM. The CPU may still hold stale
  lines for it, so a later read misses the DMAC's data unless those lines are
  **invalidated** first.

The order is the content here: fill the source and prime the destination, clean
the source, run the DMAC block copy (32-bit units, increment both, software
trigger, completion polled), invalidate the destination, then compare and park.

Both buffers are 32-byte aligned and an exact whole number of 32-byte cache
lines. That is not tidiness -- it is what makes the by-address clean and
invalidate operate on exactly the buffer, with no partial-line spill onto a
neighbouring variable.

The emulator really does move the bytes (its DMAC model performs the transfer on
the software trigger, with no CPU-memcpy fallback), so the genuine `ra8_dmac`
path runs off-target too. But its memory is byte-exact with no L1 D-cache
modelled, so the clean and invalidate calls execute and have no effect. The
hazard is observable on silicon only.

Bare EK-RA8D2, no shields or external hardware.

# canfd_filter_demo (hw_pending)

CAN-FD AFL filter test with three sub-rounds per iteration: exact
match (0x100), masked match (0x110/mask 0x7F0), and a no-match
ID (0x200) that should be filtered out.

## Status

After 7 chained chip-level fixes in commits 600b13b2 / 6fef1474 /
the canfd HAL the demo's match counter went from 5 to ~12 in
5 s windows (above the 10-match gate threshold) -- but the
mismatch counter stays at ~6 per window. That means ~2 of 3
sub-rounds match per iteration; one sub-round consistently
misbehaves. Likely the no-match round occasionally consumes a
late-arriving frame from the previous matched round; verifying
needs per-sub-round counters + JTAG dumps of CFDRFSTS during
the rx_spin.

## How to graduate back

1. Split g_canfd_filter_match into _exact, _mask, _nomatch
   counters so the failing sub-round is identifiable.
2. Add a between-sub-round drain (loop ra_canfd_receive until
   no_data) to flush any late frames before the next TX.
3. Re-run; once all three counters advance cleanly, move the dir
   back to hw_validated/hil/.

The HAL fixes that landed during the investigation (CSLPR clear,
CH_RESET-not-HALT for NCFG, RFE-after-GL_OPERATION, TMTRF clear,
RNC0 bump, GAFLFDP0 routing, TMTRF wait) all benefit any future
multi-filter CANFD work and should not be reverted.

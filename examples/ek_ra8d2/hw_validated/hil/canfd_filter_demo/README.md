# canfd_filter_demo

Exercises the CAN-FD acceptance filter in internal loopback with three
sub-rounds per iteration: an exact-ID match (0x100), a masked match (0x110 under
mask 0x7F0), and an ID that must be filtered out (0x200).

The no-match round is the one that matters and also the one that is easy to get
wrong: a frame left in the RX FIFO by the previous matched round reads exactly
like a leak through the filter. The demo drains the FIFO between sub-rounds for
that reason, and keeps per-sub-round counters (`g_canfd_filter_exact_*`,
`g_canfd_filter_mask_*`, `g_canfd_filter_nomatch_*`) alongside the aggregate
`g_canfd_filter_match` / `_mismatch`, so a failure names the sub-round rather
than just the app.

No transceiver or external harness: the on-chip CAN-FD IP is looped back to
itself. `canfd_loopback` is the same loopback with no filtering.

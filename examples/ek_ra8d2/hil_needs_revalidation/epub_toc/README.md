# epub_toc

Runs the `epub` table-of-contents path on silicon (#116) against real
`.epub` files staged on a microSD card. #74 added titled TOC parsing -- EPUB2
NCX `<navMap>` and EPUB3 `nav.xhtml` `<nav epub:type="toc">` -- but it had only
ever run on the x86 host. Building on `epub_open`, this exercises both forms
plus the malformed-TOC fallback.

Three baked books are self-provisioned onto the card if absent, opened through
`ra8_fs` and the streamed open (#230), and asserted:

- an NCX book resolves to the NCX kind with the right entry count, a byte-exact
  CRC over the first entry label, and entry 0 pointing at spine 0;
- a nav book resolves to the nav kind, likewise, with the `#fragment` stripped
  from the target;
- a book with no TOC document resolves to "none" and its spine is still
  readable -- graceful degradation, not a HardFault.

The label CRCs make this a byte-correctness gate rather than a crash test.

Like the sibling SD apps, the gate is a memprobe rather than the console: an SD
app drives the SCI0 Simple-SPI bus and an emulator that folds every SCI channel
onto one line interleaves the banner with SPI traffic. `g_etoc_heartbeat`
advances only after all three books pass, and `g_etoc_err` stamps which one
failed.

Needs a microSD in Pmod2 (J25); an unseated card is the first thing to rule out
when this fails. The fixtures are flat, so nested multi-level TOC trees are not
covered, and neither is pagination (#117) or rendering (#78).

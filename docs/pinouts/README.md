# RA8 pinout reference

Ball maps for every orderable RA8D2 and RA8P1 part number, parsed
out of section 1.7 "Pin Lists" of the two group datasheets by
`scripts/gen/gen_pinouts.py`. **These files are generated -- edit the
generator, not the output.** `gen_pinouts.py --check` runs in CI, so
a datasheet revision that moves a ball cannot land without the
reference moving with it.

## Which file do I want?

A part number's ball map is fixed by exactly two of its fields: the
**package** and whether its **feature set** bonds out MIPI DSI/CSI
(`B` and `K` do; `A` and `J` do not). Memory size, core count and
temperature grade never move a ball, so the 64 part numbers below
collapse onto 12 ball maps.

| Group | Package | MIPI DSI/CSI | Balls | I/O | Pinout file |
|---|---|---|---|---|---|
| RA8D2 | LFBGA 289 | yes | 289 | 199 | [`ra8d2_bga289_mipi.txt`](ra8d2_bga289_mipi.txt) |
| RA8D2 | LFBGA 289 | no | 289 | 208 | [`ra8d2_bga289_nomipi.txt`](ra8d2_bga289_nomipi.txt) |
| RA8D2 | LFBGA 224 | yes | 224 | 142 | [`ra8d2_bga224_mipi.txt`](ra8d2_bga224_mipi.txt) |
| RA8D2 | LFBGA 224 | no | 224 | 149 | [`ra8d2_bga224_nomipi.txt`](ra8d2_bga224_nomipi.txt) |
| RA8D2 | LFBGA 303 (SiP) | yes | 303 | 186 | [`ra8d2_bga303_sip_mipi.txt`](ra8d2_bga303_sip_mipi.txt) |
| RA8D2 | LFBGA 303 (SiP) | no | 303 | 195 | [`ra8d2_bga303_sip_nomipi.txt`](ra8d2_bga303_sip_nomipi.txt) |
| RA8P1 | LFBGA 289 | yes | 289 | 199 | [`ra8p1_bga289_mipi.txt`](ra8p1_bga289_mipi.txt) |
| RA8P1 | LFBGA 289 | no | 289 | 208 | [`ra8p1_bga289_nomipi.txt`](ra8p1_bga289_nomipi.txt) |
| RA8P1 | LFBGA 224 | yes | 224 | 142 | [`ra8p1_bga224_mipi.txt`](ra8p1_bga224_mipi.txt) |
| RA8P1 | LFBGA 224 | no | 224 | 149 | [`ra8p1_bga224_nomipi.txt`](ra8p1_bga224_nomipi.txt) |
| RA8P1 | LFBGA 303 (SiP) | yes | 303 | 186 | [`ra8p1_bga303_sip_mipi.txt`](ra8p1_bga303_sip_mipi.txt) |
| RA8P1 | LFBGA 303 (SiP) | no | 303 | 195 | [`ra8p1_bga303_sip_nomipi.txt`](ra8p1_bga303_sip_nomipi.txt) |

## Part number -> ball map

Decoded from the part-numbering scheme (Figure 1.2 of either
datasheet), cross-checked against the printed product list.

| Part number | Group | Cores | MIPI | Code memory | SRAM[^sram] | Junction temp | Package | Pinout file |
|---|---|---|---|---|---|---|---|---|
| `R7KA8D2ADLCAB` | RA8D2 | single | no | 512 KB MRAM | 1792 KB | 0 to 95 C | LFBGA 224 | [`ra8d2_bga224_nomipi.txt`](ra8d2_bga224_nomipi.txt) |
| `R7KA8D2ADLCAC` | RA8D2 | single | no | 512 KB MRAM | 1792 KB | 0 to 95 C | LFBGA 289 | [`ra8d2_bga289_nomipi.txt`](ra8d2_bga289_nomipi.txt) |
| `R7KA8D2ADDCAB` | RA8D2 | single | no | 512 KB MRAM | 1792 KB | -40 to 105 C | LFBGA 224 | [`ra8d2_bga224_nomipi.txt`](ra8d2_bga224_nomipi.txt) |
| `R7KA8D2ADDCAC` | RA8D2 | single | no | 512 KB MRAM | 1792 KB | -40 to 105 C | LFBGA 289 | [`ra8d2_bga289_nomipi.txt`](ra8d2_bga289_nomipi.txt) |
| `R7KA8D2AFLCAB` | RA8D2 | single | no | 1 MB MRAM | 1792 KB | 0 to 95 C | LFBGA 224 | [`ra8d2_bga224_nomipi.txt`](ra8d2_bga224_nomipi.txt) |
| `R7KA8D2AFLCAC` | RA8D2 | single | no | 1 MB MRAM | 1792 KB | 0 to 95 C | LFBGA 289 | [`ra8d2_bga289_nomipi.txt`](ra8d2_bga289_nomipi.txt) |
| `R7KA8D2AFDCAB` | RA8D2 | single | no | 1 MB MRAM | 1792 KB | -40 to 105 C | LFBGA 224 | [`ra8d2_bga224_nomipi.txt`](ra8d2_bga224_nomipi.txt) |
| `R7KA8D2AFDCAC` | RA8D2 | single | no | 1 MB MRAM | 1792 KB | -40 to 105 C | LFBGA 289 | [`ra8d2_bga289_nomipi.txt`](ra8d2_bga289_nomipi.txt) |
| `R7KA8D2BDLCAB` | RA8D2 | single | yes | 512 KB MRAM | 1792 KB | 0 to 95 C | LFBGA 224 | [`ra8d2_bga224_mipi.txt`](ra8d2_bga224_mipi.txt) |
| `R7KA8D2BDLCAC` | RA8D2 | single | yes | 512 KB MRAM | 1792 KB | 0 to 95 C | LFBGA 289 | [`ra8d2_bga289_mipi.txt`](ra8d2_bga289_mipi.txt) |
| `R7KA8D2BDDCAB` | RA8D2 | single | yes | 512 KB MRAM | 1792 KB | -40 to 105 C | LFBGA 224 | [`ra8d2_bga224_mipi.txt`](ra8d2_bga224_mipi.txt) |
| `R7KA8D2BDDCAC` | RA8D2 | single | yes | 512 KB MRAM | 1792 KB | -40 to 105 C | LFBGA 289 | [`ra8d2_bga289_mipi.txt`](ra8d2_bga289_mipi.txt) |
| `R7KA8D2BFLCAB` | RA8D2 | single | yes | 1 MB MRAM | 1792 KB | 0 to 95 C | LFBGA 224 | [`ra8d2_bga224_mipi.txt`](ra8d2_bga224_mipi.txt) |
| `R7KA8D2BFLCAC` | RA8D2 | single | yes | 1 MB MRAM | 1792 KB | 0 to 95 C | LFBGA 289 | [`ra8d2_bga289_mipi.txt`](ra8d2_bga289_mipi.txt) |
| `R7KA8D2BFDCAB` | RA8D2 | single | yes | 1 MB MRAM | 1792 KB | -40 to 105 C | LFBGA 224 | [`ra8d2_bga224_mipi.txt`](ra8d2_bga224_mipi.txt) |
| `R7KA8D2BFDCAC` | RA8D2 | single | yes | 1 MB MRAM | 1792 KB | -40 to 105 C | LFBGA 289 | [`ra8d2_bga289_mipi.txt`](ra8d2_bga289_mipi.txt) |
| `R7KA8D2JFLCAB` | RA8D2 | dual | no | 1 MB MRAM | 1664 KB | 0 to 95 C | LFBGA 224 | [`ra8d2_bga224_nomipi.txt`](ra8d2_bga224_nomipi.txt) |
| `R7KA8D2JFLCAC` | RA8D2 | dual | no | 1 MB MRAM | 1664 KB | 0 to 95 C | LFBGA 289 | [`ra8d2_bga289_nomipi.txt`](ra8d2_bga289_nomipi.txt) |
| `R7KA8D2JFDCAB` | RA8D2 | dual | no | 1 MB MRAM | 1664 KB | -40 to 105 C | LFBGA 224 | [`ra8d2_bga224_nomipi.txt`](ra8d2_bga224_nomipi.txt) |
| `R7KA8D2JFDCAC` | RA8D2 | dual | no | 1 MB MRAM | 1664 KB | -40 to 105 C | LFBGA 289 | [`ra8d2_bga289_nomipi.txt`](ra8d2_bga289_nomipi.txt) |
| `R7KA8D2KFLCAB` | RA8D2 | dual | yes | 1 MB MRAM | 1664 KB | 0 to 95 C | LFBGA 224 | [`ra8d2_bga224_mipi.txt`](ra8d2_bga224_mipi.txt) |
| `R7KA8D2KFLCAC` | RA8D2 | dual | yes | 1 MB MRAM | 1664 KB | 0 to 95 C | LFBGA 289 | [`ra8d2_bga289_mipi.txt`](ra8d2_bga289_mipi.txt) |
| `R7KA8D2KFDCAB` | RA8D2 | dual | yes | 1 MB MRAM | 1664 KB | -40 to 105 C | LFBGA 224 | [`ra8d2_bga224_mipi.txt`](ra8d2_bga224_mipi.txt) |
| `R7KA8D2KFDCAC` | RA8D2 | dual | yes | 1 MB MRAM | 1664 KB | -40 to 105 C | LFBGA 289 | [`ra8d2_bga289_mipi.txt`](ra8d2_bga289_mipi.txt) |
| `R7JA8D2JRLSAJ` | RA8D2 | dual | no | 5 MB (1 MB MRAM + 4 MB flash) | 1664 KB | 0 to 95 C | LFBGA 303 SiP | [`ra8d2_bga303_sip_nomipi.txt`](ra8d2_bga303_sip_nomipi.txt) |
| `R7JA8D2JSLSAJ` | RA8D2 | dual | no | 9 MB (1 MB MRAM + 8 MB flash) | 1664 KB | 0 to 95 C | LFBGA 303 SiP | [`ra8d2_bga303_sip_nomipi.txt`](ra8d2_bga303_sip_nomipi.txt) |
| `R7JA8D2JRDSAJ` | RA8D2 | dual | no | 5 MB (1 MB MRAM + 4 MB flash) | 1664 KB | -40 to 105 C | LFBGA 303 SiP | [`ra8d2_bga303_sip_nomipi.txt`](ra8d2_bga303_sip_nomipi.txt) |
| `R7JA8D2JSDSAJ` | RA8D2 | dual | no | 9 MB (1 MB MRAM + 8 MB flash) | 1664 KB | -40 to 105 C | LFBGA 303 SiP | [`ra8d2_bga303_sip_nomipi.txt`](ra8d2_bga303_sip_nomipi.txt) |
| `R7JA8D2KRLSAJ` | RA8D2 | dual | yes | 5 MB (1 MB MRAM + 4 MB flash) | 1664 KB | 0 to 95 C | LFBGA 303 SiP | [`ra8d2_bga303_sip_mipi.txt`](ra8d2_bga303_sip_mipi.txt) |
| `R7JA8D2KSLSAJ` | RA8D2 | dual | yes | 9 MB (1 MB MRAM + 8 MB flash) | 1664 KB | 0 to 95 C | LFBGA 303 SiP | [`ra8d2_bga303_sip_mipi.txt`](ra8d2_bga303_sip_mipi.txt) |
| `R7JA8D2KRDSAJ` | RA8D2 | dual | yes | 5 MB (1 MB MRAM + 4 MB flash) | 1664 KB | -40 to 105 C | LFBGA 303 SiP | [`ra8d2_bga303_sip_mipi.txt`](ra8d2_bga303_sip_mipi.txt) |
| `R7JA8D2KSDSAJ` | RA8D2 | dual | yes | 9 MB (1 MB MRAM + 8 MB flash) | 1664 KB | -40 to 105 C | LFBGA 303 SiP | [`ra8d2_bga303_sip_mipi.txt`](ra8d2_bga303_sip_mipi.txt) |
| `R7KA8P1ADLCAB` | RA8P1 | single | no | 512 KB MRAM | 1792 KB | 0 to 95 C | LFBGA 224 | [`ra8p1_bga224_nomipi.txt`](ra8p1_bga224_nomipi.txt) |
| `R7KA8P1ADLCAC` | RA8P1 | single | no | 512 KB MRAM | 1792 KB | 0 to 95 C | LFBGA 289 | [`ra8p1_bga289_nomipi.txt`](ra8p1_bga289_nomipi.txt) |
| `R7KA8P1ADDCAB` | RA8P1 | single | no | 512 KB MRAM | 1792 KB | -40 to 105 C | LFBGA 224 | [`ra8p1_bga224_nomipi.txt`](ra8p1_bga224_nomipi.txt) |
| `R7KA8P1ADDCAC` | RA8P1 | single | no | 512 KB MRAM | 1792 KB | -40 to 105 C | LFBGA 289 | [`ra8p1_bga289_nomipi.txt`](ra8p1_bga289_nomipi.txt) |
| `R7KA8P1AFLCAB` | RA8P1 | single | no | 1 MB MRAM | 1792 KB | 0 to 95 C | LFBGA 224 | [`ra8p1_bga224_nomipi.txt`](ra8p1_bga224_nomipi.txt) |
| `R7KA8P1AFLCAC` | RA8P1 | single | no | 1 MB MRAM | 1792 KB | 0 to 95 C | LFBGA 289 | [`ra8p1_bga289_nomipi.txt`](ra8p1_bga289_nomipi.txt) |
| `R7KA8P1AFDCAB` | RA8P1 | single | no | 1 MB MRAM | 1792 KB | -40 to 105 C | LFBGA 224 | [`ra8p1_bga224_nomipi.txt`](ra8p1_bga224_nomipi.txt) |
| `R7KA8P1AFDCAC` | RA8P1 | single | no | 1 MB MRAM | 1792 KB | -40 to 105 C | LFBGA 289 | [`ra8p1_bga289_nomipi.txt`](ra8p1_bga289_nomipi.txt) |
| `R7KA8P1BDLCAB` | RA8P1 | single | yes | 512 KB MRAM | 1792 KB | 0 to 95 C | LFBGA 224 | [`ra8p1_bga224_mipi.txt`](ra8p1_bga224_mipi.txt) |
| `R7KA8P1BDLCAC` | RA8P1 | single | yes | 512 KB MRAM | 1792 KB | 0 to 95 C | LFBGA 289 | [`ra8p1_bga289_mipi.txt`](ra8p1_bga289_mipi.txt) |
| `R7KA8P1BDDCAB` | RA8P1 | single | yes | 512 KB MRAM | 1792 KB | -40 to 105 C | LFBGA 224 | [`ra8p1_bga224_mipi.txt`](ra8p1_bga224_mipi.txt) |
| `R7KA8P1BDDCAC` | RA8P1 | single | yes | 512 KB MRAM | 1792 KB | -40 to 105 C | LFBGA 289 | [`ra8p1_bga289_mipi.txt`](ra8p1_bga289_mipi.txt) |
| `R7KA8P1BFLCAB` | RA8P1 | single | yes | 1 MB MRAM | 1792 KB | 0 to 95 C | LFBGA 224 | [`ra8p1_bga224_mipi.txt`](ra8p1_bga224_mipi.txt) |
| `R7KA8P1BFLCAC` | RA8P1 | single | yes | 1 MB MRAM | 1792 KB | 0 to 95 C | LFBGA 289 | [`ra8p1_bga289_mipi.txt`](ra8p1_bga289_mipi.txt) |
| `R7KA8P1BFDCAB` | RA8P1 | single | yes | 1 MB MRAM | 1792 KB | -40 to 105 C | LFBGA 224 | [`ra8p1_bga224_mipi.txt`](ra8p1_bga224_mipi.txt) |
| `R7KA8P1BFDCAC` | RA8P1 | single | yes | 1 MB MRAM | 1792 KB | -40 to 105 C | LFBGA 289 | [`ra8p1_bga289_mipi.txt`](ra8p1_bga289_mipi.txt) |
| `R7KA8P1JFLCAB` | RA8P1 | dual | no | 1 MB MRAM | 1664 KB | 0 to 95 C | LFBGA 224 | [`ra8p1_bga224_nomipi.txt`](ra8p1_bga224_nomipi.txt) |
| `R7KA8P1JFLCAC` | RA8P1 | dual | no | 1 MB MRAM | 1664 KB | 0 to 95 C | LFBGA 289 | [`ra8p1_bga289_nomipi.txt`](ra8p1_bga289_nomipi.txt) |
| `R7KA8P1JFDCAB` | RA8P1 | dual | no | 1 MB MRAM | 1664 KB | -40 to 105 C | LFBGA 224 | [`ra8p1_bga224_nomipi.txt`](ra8p1_bga224_nomipi.txt) |
| `R7KA8P1JFDCAC` | RA8P1 | dual | no | 1 MB MRAM | 1664 KB | -40 to 105 C | LFBGA 289 | [`ra8p1_bga289_nomipi.txt`](ra8p1_bga289_nomipi.txt) |
| `R7KA8P1KFLCAB` | RA8P1 | dual | yes | 1 MB MRAM | 1664 KB | 0 to 95 C | LFBGA 224 | [`ra8p1_bga224_mipi.txt`](ra8p1_bga224_mipi.txt) |
| `R7KA8P1KFLCAC` | RA8P1 | dual | yes | 1 MB MRAM | 1664 KB | 0 to 95 C | LFBGA 289 | [`ra8p1_bga289_mipi.txt`](ra8p1_bga289_mipi.txt) |
| `R7KA8P1KFDCAB` | RA8P1 | dual | yes | 1 MB MRAM | 1664 KB | -40 to 105 C | LFBGA 224 | [`ra8p1_bga224_mipi.txt`](ra8p1_bga224_mipi.txt) |
| `R7KA8P1KFDCAC` | RA8P1 | dual | yes | 1 MB MRAM | 1664 KB | -40 to 105 C | LFBGA 289 | [`ra8p1_bga289_mipi.txt`](ra8p1_bga289_mipi.txt) |
| `R7JA8P1JRLSAJ` | RA8P1 | dual | no | 5 MB (1 MB MRAM + 4 MB flash) | 1664 KB | 0 to 95 C | LFBGA 303 SiP | [`ra8p1_bga303_sip_nomipi.txt`](ra8p1_bga303_sip_nomipi.txt) |
| `R7JA8P1JSLSAJ` | RA8P1 | dual | no | 9 MB (1 MB MRAM + 8 MB flash) | 1664 KB | 0 to 95 C | LFBGA 303 SiP | [`ra8p1_bga303_sip_nomipi.txt`](ra8p1_bga303_sip_nomipi.txt) |
| `R7JA8P1JRDSAJ` | RA8P1 | dual | no | 5 MB (1 MB MRAM + 4 MB flash) | 1664 KB | -40 to 105 C | LFBGA 303 SiP | [`ra8p1_bga303_sip_nomipi.txt`](ra8p1_bga303_sip_nomipi.txt) |
| `R7JA8P1JSDSAJ` | RA8P1 | dual | no | 9 MB (1 MB MRAM + 8 MB flash) | 1664 KB | -40 to 105 C | LFBGA 303 SiP | [`ra8p1_bga303_sip_nomipi.txt`](ra8p1_bga303_sip_nomipi.txt) |
| `R7JA8P1KRLSAJ` | RA8P1 | dual | yes | 5 MB (1 MB MRAM + 4 MB flash) | 1664 KB | 0 to 95 C | LFBGA 303 SiP | [`ra8p1_bga303_sip_mipi.txt`](ra8p1_bga303_sip_mipi.txt) |
| `R7JA8P1KSLSAJ` | RA8P1 | dual | yes | 9 MB (1 MB MRAM + 8 MB flash) | 1664 KB | 0 to 95 C | LFBGA 303 SiP | [`ra8p1_bga303_sip_mipi.txt`](ra8p1_bga303_sip_mipi.txt) |
| `R7JA8P1KRDSAJ` | RA8P1 | dual | yes | 5 MB (1 MB MRAM + 4 MB flash) | 1664 KB | -40 to 105 C | LFBGA 303 SiP | [`ra8p1_bga303_sip_mipi.txt`](ra8p1_bga303_sip_mipi.txt) |
| `R7JA8P1KSDSAJ` | RA8P1 | dual | yes | 9 MB (1 MB MRAM + 8 MB flash) | 1664 KB | -40 to 105 C | LFBGA 303 SiP | [`ra8p1_bga303_sip_mipi.txt`](ra8p1_bga303_sip_mipi.txt) |

[^sram]: SRAM is the one column here that is not pinout data and not read
    per-part: the Function Comparison table merges it across columns, giving
    1792 KB for the single-core feature sets (`A`, `B`) and 1664 KB for the
    dual-core ones (`J`, `K`), the latter spending 128 KB on the CM33 TCM.
    Every SiP part is dual-core. Take it as orientation and confirm against
    the datasheet before sizing anything against it.

## Reading a part number

```
R 7 K A 8 D 2 A D L C AB
          | | | | | |  +-- package: AB=LFBGA224 AC=LFBGA289 AJ=LFBGA303
          | | | | | +----- quality grade: C=standard S=SiP
          | | | | +------- junction temp: L=0..95C D=-40..105C
          | | | +--------- code memory: D=512KB F=1MB R=1MB+4MB S=1MB+8MB
          | | +----------- feature set: A/B single core, J/K dual core;
          | |              B/K bond out MIPI DSI/CSI, A/J do not
          +-+------------- group: D2=RA8D2, P1=RA8P1
```

The leading `R7K`/`R7J` also encodes the memory technology (`K`=MRAM,
`J`=MRAM+flash SiP) and so tracks the quality-grade and package
fields; the generator rejects a part number where the three
disagree.

## Pin compatibility between the two groups

- **Standard products: identical.** Every ball of every Standard package carries the same function set on the RA8D2 and the RA8P1 -- 1026 (variant, ball) pairs compared, established by diffing the two parsed pin lists rather than by assertion.
- **SiP products: identical.** Every ball of every SiP package carries the same function set on the RA8D2 and the RA8P1 -- 606 (variant, ball) pairs compared, established by diffing the two parsed pin lists rather than by assertion.

The one function the two groups do not share (the RA8P1's Ethos-U55
NPU) is not pinned out, so pin compatibility is what the diff above
shows. See `docs/reference/ra8p1_vs_ra8d2.md` for the register-level delta.

## Sources

| Group | Datasheet | Committed as |
|---|---|---|
| RA8D2 | R01DS0493EJ | `docs/reference/ra8d2-datasheet.pdf` |
| RA8P1 | R01DS0439EJ | `docs/reference/ra8p1-datasheet.pdf` |

The Hardware User's Manual, not the datasheet, is the authority on
*register* programming for any of these pins -- see
`docs/reference/README.md`. The datasheet is the authority on which
ball carries which function, which is what these files record.


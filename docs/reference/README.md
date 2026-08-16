# Reference Material

Committed PDFs for quick offline reference while writing HAL code. All files
come directly from Renesas and are reproduced here under their respective
original terms.

| File | Renesas doc # | Purpose |
|------|---------------|---------|
| `ra8d2-datasheet.pdf` | R01DS0493EJ0130 | Electrical specifications, pin lists, peripheral summary. First stop when deciding which pins a peripheral can land on. |
| `ra8d2-hardware-user-manual.pdf` | R01UH1065EJ0130 | **Primary register reference.** Use this when writing any HAL code. Cite page / section numbers in commit messages. |
| `ra8d2-technical-brief.pdf` | R01TB0104EJ0100 | High-level overview. Good for understanding core block diagrams. |
| `ra8d2-high-temperature-operation.pdf` | R01AN8060EJ0100 | Application note for high-temperature operating conditions. |
| `ra8p1-datasheet.pdf` | R01DS0439EJ0130 | RA8P1 group (RA8D2 + Ethos-U55 NPU). Committed so the RA8P1 half of `docs/pinouts/` is reproducible from the tree and so a claim about the RA8P1 can be re-checked here rather than taken on trust. The RA8P1 Hardware User's Manual (R01UH1064EJ0130) is ~49 MB and is NOT committed. |

Per-package ball maps for every orderable RA8D2 and RA8P1 part number are
parsed out of the two datasheets above into [`docs/pinouts/`](../pinouts/)
by `scripts/gen/gen_pinouts.py`. Read those rather than the datasheet PDF
when the question is "which ball, and what else can it be".

## External resources

- **FSP source** (for reference only, do not copy code):
  https://github.com/renesas/fsp
- **FSP BSP documentation**:
  https://renesas.github.io/fsp/group___b_s_p___m_c_u.html
- **FSP product page**:
  https://www.renesas.com/en/software-tool/ra-flexible-software-package-fsp
- **RA8D2 product page**:
  https://www.renesas.com/en/products/ra8d2
- **RA8P1 product page**:
  https://www.renesas.com/en/products/ra8p1
- **Keil CMSIS DFP for RA**:
  https://www.keil.arm.com/packs/ra_dfp-renesas/versions/

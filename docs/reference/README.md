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

## External resources

- **FSP source** (for reference only, do not copy code):
  https://github.com/renesas/fsp
- **FSP BSP documentation**:
  https://renesas.github.io/fsp/group___b_s_p___m_c_u.html
- **FSP product page**:
  https://www.renesas.com/en/software-tool/ra-flexible-software-package-fsp
- **RA8D2 product page**:
  https://www.renesas.com/en/products/ra8d2
- **Keil CMSIS DFP for RA**:
  https://www.keil.arm.com/packs/ra_dfp-renesas/versions/

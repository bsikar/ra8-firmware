# Project Rules for ereader_rev1

## Library Import Standards
- All imported schematic symbols, footprint libraries, and 3D models must adhere to [LIBRARY_STANDARDS.md](../LIBRARY_STANDARDS.md).
- Keep components in the functional libraries under `libs/symbols`, `libs/footprints`, and `libs/3dmodels`; never import them into global user libraries.
- Verify and correct symbol designators (e.g. use `U` instead of `IC` for microcontrollers and complex ICs).
- Verify 3D model paths in footprints use `${KIPRJMOD}` variables rather than absolute or empty paths.

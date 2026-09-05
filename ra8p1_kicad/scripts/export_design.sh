#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Export the saved, complete schematic hierarchy to a review PDF.
# Usage: export_design.sh [path/to/project.kicad_sch]
# KICAD_CLI optionally selects an exact executable, including paths with spaces.
# A failed export leaves the previous review PDF intact.

set -eu

case "${1-}" in
-h | --help)
  printf '%s\n' 'Usage: export_design.sh [path/to/project.kicad_sch]' \
    'Default: ereader/ereader_rev1.kicad_sch' \
    'Output: ra8p1_kicad/exports/<schematic-name>.pdf' \
    'Set KICAD_CLI to override KiCad executable discovery.' \
    'Save all sheets in KiCad before exporting.'
  exit 0
  ;;
esac

[ "$#" -le 1 ] || {
  printf '%s\n' 'Error: expected at most one schematic path.' >&2
  exit 2
}
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
hardware_dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
schematic=${1:-"$hardware_dir/ereader/ereader_rev1.kicad_sch"}
case "$schematic" in
*.kicad_sch) ;;
*)
  printf '%s\n' 'Error: input must be a .kicad_sch file.' >&2
  exit 2
  ;;
esac
[ -f "$schematic" ] || {
  printf 'Error: schematic not found: %s\n' "$schematic" >&2
  exit 2
}
project_dir=$(CDPATH='' cd -- "$(dirname -- "$schematic")" && pwd)
filename=$(basename -- "$schematic")
name=${filename%.kicad_sch}

if [ -n "${KICAD_CLI:-}" ]; then
  cli=$KICAD_CLI
elif command -v kicad-cli >/dev/null 2>&1; then
  cli=$(command -v kicad-cli)
elif [ -x /Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli ]; then
  cli=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
else
  printf '%s\n' 'Error: install KiCad or set KICAD_CLI to its CLI executable.' >&2
  exit 127
fi
# Resolve relative overrides before switching to the project directory.
cli=$(command -v "$cli") || {
  printf '%s\n' 'Error: KICAD_CLI is not executable.' >&2
  exit 127
}
case "$cli" in
/*) ;;
*) cli="$(pwd)/$cli" ;;
esac

output_dir="$hardware_dir/exports"
mkdir -p "$output_dir"
temp_dir=$(mktemp -d "$output_dir/.export-$name.XXXXXX")
temp_pdf="$temp_dir/$name.pdf"
# Remove only the known temporary output and its now-empty directory.
trap 'rm -f "$temp_pdf"; rmdir "$temp_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

cd -- "$project_dir"
"$cli" sch export pdf --no-background-color --output "$temp_pdf" "$project_dir/$filename"
[ -s "$temp_pdf" ] || {
  printf '%s\n' 'Error: KiCad produced no PDF.' >&2
  exit 1
}
[ "$(head -c 5 "$temp_pdf")" = '%PDF-' ] || {
  printf '%s\n' 'Error: output is not a PDF.' >&2
  exit 1
}
mv -f "$temp_pdf" "$output_dir/$name.pdf"
printf 'Exported all schematic sheets: %s\n' "$output_dir/$name.pdf"

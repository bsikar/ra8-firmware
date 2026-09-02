#!/usr/bin/env sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -eu

tool_root=$1
binary=$2
source_pattern='(^|[^[:alnum:]_])(FILE|DIR|stdio|malloc|calloc|realloc|free|strdup|mmap|munmap)([^[:alnum:]_]|$)'
symbol_pattern='[[:space:]]_?(fopen|fdopen|freopen|fclose|fread|fwrite|fprintf|printf|snprintf|sprintf|fputc|fflush|fseek|ftell|tmpfile|opendir|fdopendir|readdir|closedir|scandir|malloc|calloc|realloc|free|strdup|aligned_alloc|posix_memalign|mmap|munmap)(@[^[:space:]]+)?$'

for dir in "$tool_root/inc" "$tool_root/src"; do
  [ -d "$dir" ] || {
    echo "check_production_surface.sh: not a directory: $dir" >&2
    exit 2
  }
done

# Non-vacuity floor: a clean surface report only means something if files were
# actually scanned. A collapsed scan must fail, not read as clean.
scanned=$(find "$tool_root/inc" "$tool_root/src" -type f \( -name '*.c' -o -name '*.h' \) \
  -print | wc -l | tr -d '[:space:]')
if [ "$scanned" -eq 0 ]; then
  echo "check_production_surface.sh: 0 production sources under $tool_root;" \
    "a clean report here would be vacuous" >&2
  exit 2
fi

# grep's status is read rather than masked: 0 = matches printed, 1 = nothing
# matched, anything above 1 = grep itself failed. Only 1 may be reported as a
# clean production surface. The old `|| true` on a find -exec grep conflated
# all three, so a bad tool_root passed the check silently.
set +e
source_findings=$(grep -EnHr --include='*.c' --include='*.h' "$source_pattern" \
  "$tool_root/inc" "$tool_root/src")
grep_status=$?
set -e
if [ "$grep_status" -gt 1 ]; then
  echo "check_production_surface.sh: source scan failed (grep exit $grep_status)" >&2
  exit 2
fi
if [ -n "$source_findings" ]; then
  printf '%s\n' "$source_findings"
  echo "forbidden hosted-stream, allocator, or mapping token in production source" >&2
  exit 1
fi

if nm -u "$binary" | grep -E "$symbol_pattern"; then
  echo "forbidden undefined production symbol" >&2
  exit 1
fi

exit 0

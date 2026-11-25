#!/usr/bin/env sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  echo "usage: check_production_surface.sh <tool-root> <viewer-bin> <reader-archive> [portable-test-bin]" >&2
  exit 2
fi

tool_root=$1
binary=$2
reader_archive=$3
portable_test=${4:-}

source_pattern='(^|[^[:alnum:]_])(FILE|DIR|stdio|malloc|calloc|realloc|free|strdup|mmap|munmap)([^[:alnum:]_]|$)'
symbol_pattern='[[:space:]]_?(fopen|fdopen|freopen|fclose|fread|fwrite|fprintf|printf|snprintf|sprintf|fputc|fflush|fseek|ftell|tmpfile|opendir|fdopendir|readdir|closedir|scandir|malloc|calloc|realloc|free|strdup|aligned_alloc|posix_memalign|mmap|munmap)(@[^[:space:]]+)?$'
posix_hdr_pattern='^[[:space:]]*#[[:space:]]*include[[:space:]]*<(fcntl\.h|unistd\.h|sys/|errno\.h|stdio\.h|stdlib\.h)>'
posix_sym_pattern='[[:space:]]_?(open|open64|openat|openat64|pread|pread64|pwrite|pwrite64|read|write|close|fstat|fstatat|mmap|mmap64|munmap|malloc|calloc|realloc|free)(@[^[:space:]]+)?$'

for dir in "$tool_root/inc" "$tool_root/src"; do
  [ -d "$dir" ] || {
    echo "check_production_surface.sh: not a directory: $dir" >&2
    exit 2
  }
done

# Non-vacuity floor: ensure files were actually scanned.
scanned=$(find "$tool_root/inc" "$tool_root/src" -type f \( -name '*.c' -o -name '*.h' \) \
  -print | wc -l | tr -d '[:space:]')
if [ "$scanned" -eq 0 ]; then
  echo "check_production_surface.sh: 0 production sources under $tool_root;" \
    "a clean report here would be vacuous" >&2
  exit 2
fi

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
  echo "forbidden hosted-stream, allocator, or mapping token in viewer source" >&2
  exit 1
fi

# Portable core TUs must remain free of POSIX descriptor and stdlib/stdio headers.
for core in \
  "$tool_root/inc/ra8_viewer_reader.h" \
  "$tool_root/inc/ra8_viewer_reader_internal.h" \
  "$tool_root/inc/ra8_viewer_output_internal.h" \
  "$tool_root/src/ra8_viewer_reader.c" \
  "$tool_root/src/ra8_viewer_comic.c" \
  "$tool_root/src/ra8_viewer_jof.c" \
  "$tool_root/src/ra8_viewer_reflow.c" \
  "$tool_root/src/ra8_viewer_output.c"; do
  [ -f "$core" ] || {
    echo "check_production_surface.sh: missing portable core unit: $core" >&2
    exit 2
  }
  if grep -En "$posix_hdr_pattern" "$core"; then
    echo "forbidden POSIX/stdlib/stdio header in portable core unit $core" >&2
    exit 1
  fi
done

if nm -u "$binary" | grep -E "$symbol_pattern"; then
  echo "forbidden undefined production symbol in $binary" >&2
  exit 1
fi

if nm -u "$reader_archive" | grep -E "$symbol_pattern"; then
  echo "forbidden undefined symbol in portable reader archive $reader_archive" >&2
  exit 1
fi

if [ -n "$portable_test" ]; then
  if nm -u "$portable_test" | grep -E "$posix_sym_pattern"; then
    echo "forbidden POSIX or heap symbol in portable arena binary $portable_test" >&2
    exit 1
  fi
fi

exit 0

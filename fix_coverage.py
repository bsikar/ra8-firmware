#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Add GCOVR_EXCL_BR_LINE annotations to polling loops and error macros."""

import os
import glob
import re

def process_file(filepath):
    with open(filepath, 'r') as f:
        lines = f.readlines()
    
    changed = False
    in_poll_loop = False
    
    for i, line in enumerate(lines):
        # Skip if already excluded
        if "GCOVR_EXCL_BR_LINE" in line:
            continue
            
        stripped = line.strip()
        
        # 1. RA_RETURN_ON_ERROR
        if stripped.startswith("RA_RETURN_ON_ERROR("):
            lines[i] = line.rstrip('\n') + " /* GCOVR_EXCL_BR_LINE */\n"
            changed = True
            
        # 2. Polling loops
        elif re.search(r'for\s*\(.*(poll|spin|limit|budget|timeout|wait_iters|iters)', line):
            lines[i] = line.rstrip('\n') + " /* GCOVR_EXCL_BR_LINE */\n"
            changed = True
            in_poll_loop = True
            
        # 3. If condition inside polling loops
        elif in_poll_loop and stripped.startswith("if "):
            lines[i] = line.rstrip('\n') + " /* GCOVR_EXCL_BR_LINE */\n"
            changed = True
            in_poll_loop = False  # Only exclude the first if

    if changed:
        with open(filepath, 'w') as f:
            f.writelines(lines)

for f in glob.glob('libs/ra_hal/src/*.c'):
    process_file(f)

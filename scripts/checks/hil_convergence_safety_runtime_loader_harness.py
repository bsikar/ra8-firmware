# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Private loader harness text and subprocess boundary."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

_HARNESS = (
    "import errno, os, pathlib, sys\n"
    "main_path, process_path, cases_path = map(pathlib.Path, sys.argv[1:])\n"
    "main_source = main_path.read_text(encoding='utf-8')\n"
    "cases_source = cases_path.read_text(encoding='utf-8')\n"
    "globals()['__name__'] = '_ra8_supervisor_reexec'\n"
    "globals()['__file__'] = str(main_path)\n"
    "sys.modules['_ra8_supervisor_reexec'] = sys.modules['__main__']\n"
    "exec(compile(main_source, str(main_path), 'exec'), globals())\n"
    "process_name = '_ra8_supervisor_process'\n"
    "sentinel = object()\n"
    "sys.modules[process_name] = sentinel\n"
    "descriptor = os.open(process_path, os.O_RDONLY | os.O_NOFOLLOW)\n"
    "try:\n"
    "    globals()['_load_process_api'](descriptor)\n"
    "except RuntimeError as error:\n"
    "    if str(error) != 'supervisor process module name is already occupied':\n"
    "        raise SystemExit(7) from error\n"
    "else:\n"
    "    raise SystemExit(8)\n"
    "try:\n"
    "    os.fstat(descriptor)\n"
    "except OSError as error:\n"
    "    if error.errno != errno.EBADF:\n"
    "        raise SystemExit(11) from error\n"
    "else:\n"
    "    raise SystemExit(12)\n"
    "if sys.modules.get(process_name) is not sentinel:\n"
    "    raise SystemExit(9)\n"
    "del sys.modules[process_name]\n"
    "for _attempt in range(2):\n"
    "    descriptor = os.open(process_path, os.O_RDONLY | os.O_NOFOLLOW)\n"
    "    api = globals()['_load_process_api'](descriptor)\n"
    "    try:\n"
    "        os.fstat(descriptor)\n"
    "    except OSError as error:\n"
    "        if error.errno != errno.EBADF:\n"
    "            raise SystemExit(13) from error\n"
    "    else:\n"
    "        raise SystemExit(14)\n"
    "    if process_name in sys.modules:\n"
    "        raise SystemExit(10)\n"
    "    globals()['_install_process_api'](api)\n"
    "namespace = {'__name__': '_ra8_supervisor_cases', "
    "'__file__': str(cases_path), '_RA8_SUPERVISOR_CASES_VERSION': 1}\n"
    "compiled = compile(cases_source, str(cases_path), 'exec')\n"
    "exec(compiled, namespace)\n"
    "grant = namespace.pop('_RA8_SUPERVISOR_CASES_VERSION', None)\n"
    "if grant != 1 or '_RA8_SUPERVISOR_CASES_VERSION' in namespace:\n"
    "    raise SystemExit(3)\n"
    "dispatch = namespace.get('dispatch_supervisor_cases')\n"
    "if not callable(dispatch) or dispatch.__globals__ is not namespace:\n"
    "    raise SystemExit(2)\n"
    "try:\n"
    "    exec(compiled, dispatch.__globals__)\n"
    "except RuntimeError as error:\n"
    "    if str(error) != 'supervisor cases module is source-only':\n"
    "        raise SystemExit(4) from error\n"
    "else:\n"
    "    raise SystemExit(5)\n"
    "if '_RA8_SUPERVISOR_CASES_VERSION' in namespace:\n"
    "    raise SystemExit(6)\n"
)


def run(
    main_path: Path, process_path: Path, cases_path: Path, timeout: float
) -> subprocess.CompletedProcess[bytes]:
    """Execute the fixed private-API loader harness once."""
    return subprocess.run(  # noqa: S603 -- current interpreter and fixed private test sources
        (
            sys.executable,
            "-B",
            "-I",
            "-S",
            "-c",
            _HARNESS,
            str(main_path),
            str(process_path),
            str(cases_path),
        ),
        capture_output=True,
        timeout=timeout,
        check=False,
    )

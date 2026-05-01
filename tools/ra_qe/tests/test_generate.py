#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
"""test_generate.py -- host-side unittest for tools/ra_qe/generate.py.

Runs generate.py on the sample blink config in a fresh tempdir, then
shells out to arm-none-eabi-gcc -c on each generated .c file. If the
cross-compiler is not installed the compile step is skipped (the test
still asserts the generator produced the expected files).
"""
from __future__ import annotations

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

THIS_DIR = pathlib.Path(__file__).resolve().parent
RA_QE_DIR = THIS_DIR.parent
SAMPLE_CONFIG = RA_QE_DIR / "examples" / "blink_config.json"
GENERATE = RA_QE_DIR / "generate.py"

EXPECTED_FILES = {
    "gen_clocks.c", "gen_clocks.h",
    "gen_pinmux.c", "gen_pinmux.h",
    "gen_irq.c", "gen_irq.h",
    "gen_mstp.c", "gen_mstp.h",
}


class GenerateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="ra_qe_test_"))
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))

    def _run_generator(self, config_path: pathlib.Path) -> pathlib.Path:
        out_dir = self.tmp / "gen"
        result = subprocess.run(
            [
                sys.executable, str(GENERATE),
                "--config", str(config_path),
                "--output-dir", str(out_dir),
            ],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(
            result.returncode, 0,
            msg=f"generate.py failed: {result.stderr}\n{result.stdout}",
        )
        return out_dir

    def test_blink_sample_emits_expected_files(self) -> None:
        out_dir = self._run_generator(SAMPLE_CONFIG)
        produced = {p.name for p in out_dir.iterdir()}
        self.assertEqual(produced, EXPECTED_FILES)

    def test_blink_sample_files_have_doxygen_and_pragma_once(self) -> None:
        out_dir = self._run_generator(SAMPLE_CONFIG)
        for name in EXPECTED_FILES:
            text = (out_dir / name).read_text(encoding="utf-8")
            self.assertIn("@brief", text, msg=f"{name} missing @brief")
            self.assertIn("Brighton Sikarskie", text, msg=f"{name} missing copyright")
            self.assertIn("SPDX-License-Identifier: MIT", text)
            if name.endswith(".h"):
                self.assertIn("#pragma once", text, msg=f"{name} missing #pragma once")

    def test_invalid_config_rejected(self) -> None:
        bad = self.tmp / "bad.json"
        bad.write_text(json.dumps({"app": "x"}), encoding="utf-8")
        result = subprocess.run(
            [
                sys.executable, str(GENERATE),
                "--config", str(bad),
                "--output-dir", str(self.tmp / "gen2"),
            ],
            capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing required key", result.stderr)

    def test_generated_c_compiles_with_arm_none_eabi_gcc(self) -> None:
        gcc = shutil.which("arm-none-eabi-gcc")
        if not gcc:
            self.skipTest("arm-none-eabi-gcc not installed; skipping compile check")
        out_dir = self._run_generator(SAMPLE_CONFIG)
        for c_file in sorted(out_dir.glob("*.c")):
            obj = out_dir / (c_file.stem + ".o")
            cmd = [
                gcc, "-c",
                "-std=c23",
                "-mcpu=cortex-m85",
                "-mthumb",
                "-Wall", "-Wextra", "-Werror",
                "-ffreestanding",
                "-I", str(out_dir),
                "-o", str(obj),
                str(c_file),
            ]
            result = subprocess.run(cmd, capture_output=True, text=True, check=False)
            self.assertEqual(
                result.returncode, 0,
                msg=f"compile failed for {c_file.name}:\n"
                    f"cmd: {' '.join(cmd)}\n"
                    f"stderr:\n{result.stderr}\n"
                    f"stdout:\n{result.stdout}",
            )
            self.assertTrue(obj.exists(), msg=f"{obj} not produced")


if __name__ == "__main__":
    unittest.main()

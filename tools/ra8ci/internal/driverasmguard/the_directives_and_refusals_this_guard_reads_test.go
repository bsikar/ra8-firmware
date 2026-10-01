// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package driverasmguard

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// driverDir plants the one directory this gate scans and returns the root.
func driverDir(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	dir := filepath.Join(root, "libs", "ra8_hal", "src")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for name, body := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

// guarded is one invocation of Run over a planted root.
type guarded struct {
	code   int
	stdout string
	stderr string
}

func guard(t *testing.T, ctx context.Context, root string, args ...string) guarded {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, args, &stdout, &stderr)
	return guarded{code: code, stdout: stdout.String(), stderr: stderr.String()}
}

// A directory of drivers that guard no asm passes, and the pass says how many
// translation units were actually read. A gate that reports PASS without a
// count cannot be told apart from one that scanned nothing.
func TestACleanDriverDirectoryPassesAndSaysHowManyItRead(t *testing.T) {
	root := driverDir(t, map[string]string{
		"ra8_gpio.c": "#ifdef RA8_OFF_TARGET\nvoid f(void) { ra8_hw_wfi(); }\n#endif\n",
		"ra8_spi.c":  "// __asm__(\"nop\") named in prose is not asm\nvoid g(void) { ra8_hw_nop(); }\n",
	})
	got := guard(t, context.Background(), root)
	if got.code != 0 || got.stderr != "" {
		t.Fatalf("a clean driver directory answered %+v", got)
	}
	if !strings.Contains(got.stdout, "PASS") || !strings.Contains(got.stdout, "2 HAL driver TU(s)") {
		t.Fatalf("the pass did not say how many drivers it read: %q", got.stdout)
	}
}

// A driver the gate cannot read is a refusal naming the file, not a pass over
// the drivers it could read. The count in the pass line is what makes that
// distinction matter.
func TestADriverTheGateCannotReadIsARefusal(t *testing.T) {
	root := driverDir(t, map[string]string{
		"ra8_gpio.c": "void f(void) { ra8_hw_wfi(); }\n",
		"ra8_spi.c":  "void g(void) { ra8_hw_nop(); }\n",
	})
	sealed := filepath.Join(root, "libs", "ra8_hal", "src", "ra8_spi.c")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o600) })
	if _, err := os.ReadFile(sealed); err == nil {
		t.Skip("this process can read a sealed file")
	}

	got := guard(t, context.Background(), root)
	if got.code != 2 {
		t.Fatalf("a sealed driver answered %+v", got)
	}
	if !strings.Contains(got.stderr, "ra8_spi.c") || !strings.Contains(got.stderr, "cannot read") {
		t.Fatalf("the refusal did not name the driver at fault: %q", got.stderr)
	}
	if strings.Contains(got.stdout, "PASS") {
		t.Fatalf("a refused scan still announced a pass: %q", got.stdout)
	}
}

// A cancelled scan is refused before any verdict. The check sits at the head of
// the per-file loop, so a cancelled context is caught with drivers still
// unread rather than after a partial answer.
func TestACancelledScanIsRefusedRatherThanPassed(t *testing.T) {
	root := driverDir(t, map[string]string{"ra8_gpio.c": "void f(void) { ra8_hw_wfi(); }\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	got := guard(t, ctx, root)
	if got.code != 2 {
		t.Fatalf("a cancelled scan answered %+v", got)
	}
	if !strings.Contains(got.stderr, "cancelled") {
		t.Fatalf("the refusal did not say it was cancelled: %q", got.stderr)
	}
	if strings.Contains(got.stdout, "PASS") {
		t.Fatalf("a cancelled scan still announced a pass: %q", got.stdout)
	}
}

// The directive reader matches the Python gate's word boundary: a keyword
// followed by punctuation is that keyword, a keyword followed by more
// identifier is a different word, and a line that is a bare hash is not a
// directive at all. Anything else with a name is read as the directive it
// names, which is how #pragma and friends reach the default branch rather
// than opening a conditional.
func TestTheDirectiveReaderKeepsItsWordBoundary(t *testing.T) {
	for _, one := range []struct {
		line       string
		directive  string
		expression string
		ok         bool
	}{
		{"#if(defined(RA8_OFF_TARGET))", "if", "(defined(RA8_OFF_TARGET))", true},
		{"#ifdef RA8_OFF_TARGET", "ifdef", "RA8_OFF_TARGET", true},
		{"#iffy RA8_OFF_TARGET", "iffy", "RA8_OFF_TARGET", true},
		{"#ifdefined", "ifdefined", "", true},
		{"#pragma once", "pragma", "once", true},
		{"#endif", "endif", "", true},
		{"#", "", "", false},
		{"#   ", "", "", false},
		{"  not a directive", "", "", false},
	} {
		directive, expression, ok := parseDirective(one.line)
		if ok != one.ok || directive != one.directive || expression != one.expression {
			t.Fatalf("%q read as (%q, %q, %v), want (%q, %q, %v)",
				one.line, directive, expression, ok, one.directive, one.expression, one.ok)
		}
	}
}

// The boundary is not academic: a conditional opened with punctuation still
// guards what follows, and a word that merely starts with a keyword opens
// nothing, so asm under it is asm nobody guarded.
func TestAConditionalOpenedWithPunctuationStillGuards(t *testing.T) {
	guardedAsm := checkSource("libs/ra8_hal/src/ra8_gpio.c",
		"#if(defined(RA8_OFF_TARGET))\n__asm(\"nop\");\n#endif\n")
	if len(guardedAsm) != 1 || guardedAsm[0].line != 2 {
		t.Fatalf("asm under #if(...) was not reported: %+v", guardedAsm)
	}
	unopened := checkSource("libs/ra8_hal/src/ra8_gpio.c",
		"#iffy RA8_OFF_TARGET\n__asm(\"nop\");\n#endif\n")
	if len(unopened) != 0 {
		t.Fatalf("asm under a word that only starts like a keyword was reported: %+v", unopened)
	}
}

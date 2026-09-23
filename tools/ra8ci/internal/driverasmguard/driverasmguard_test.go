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

func TestCheckSourceMatchesConditionalAsmContract(t *testing.T) {
	source := `#if defined(OTHER)
void ordinary(void) { __asm("not guarded by the host flag"); }
#elif DEFINED(RA8_OFF_TARGET)
void guarded_a(void) { __asm__("nop"); }
#else
#if RA8_OFF_TARGET == 1
void guarded_b(void) { prefix__asm("not a token"); __asm("wfi"); }
#endif
#endif
#if(RA8_OFF_TARGET)
void guarded_c(void) { __asm("parenthesized directive"); }
#endif
// __asm__("comment") is not code.
`
	got := checkSource("libs/ra8_hal/src/test.c", source)
	if len(got) != 3 || got[0].line != 4 || got[1].line != 7 || got[2].line != 11 {
		t.Fatalf("findings = %+v, want exactly guarded asm at lines 4, 7, and 11", got)
	}
}

func TestStripCommentsPreservesLineCountAndBlockState(t *testing.T) {
	got := stripComments("__asm(\"nop\"); /* __asm__(x)\ncontinued */ __asm__(\"wfi\"); // __asm__\n")
	if len(got) != 3 || !hasInlineAsm(got[0]) || !hasInlineAsm(got[1]) || got[2] != "" {
		t.Fatalf("stripped lines = %#v", got)
	}
}

func TestRunSelftestAndWholeDirectory(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr); code != 0 || stderr.Len() != 0 {
		t.Fatalf("selftest code=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	root := t.TempDir()
	dir := filepath.Join(root, "libs", "ra8_hal", "src")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "driver.c"), []byte("#ifdef RA8_OFF_TARGET\n__asm(\"nop\");\n#endif\n"), 0600); err != nil {
		t.Fatal(err)
	}
	stdout.Reset()
	stderr.Reset()
	if code := Run(context.Background(), root, nil, &stdout, &stderr); code != 1 || !strings.Contains(stdout.String(), "driver.c:2") {
		t.Fatalf("scan code=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

func TestRunRejectsMissingScopeAndArguments(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), t.TempDir(), nil, &stdout, &stderr); code != 1 {
		t.Fatalf("missing scan root code=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	stdout.Reset()
	stderr.Reset()
	if code := Run(context.Background(), t.TempDir(), []string{"--all"}, &stdout, &stderr); code != 2 {
		t.Fatalf("unreviewed argument code=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

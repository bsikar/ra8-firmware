// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gnuattribute

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"
)

func TestScanDetectionAndExemptions(t *testing.T) {
	tests := []struct {
		name   string
		source string
		want   int
	}{
		{"weak", "void f(void) __attribute__((weak));\n", 1},
		{"packed dunder", "int x __attribute__((__packed__));\n", 1},
		{"allowed attributes", "void a(void) __attribute__((interrupt));\nvoid b(void) __attribute__((__cmse_nonsecure_entry__, cmse_nonsecure_call));\n", 0},
		{"waiver", "int x __attribute__((packed)); // ATTR-OK: hardware ABI\n", 0},
		{"line comment", "// __attribute__((weak))\n", 0},
		{"block comment", "/* __attribute__((weak)) */\nint x;\n", 0},
		{"string and character", "const char *s = \"__attribute__((weak))\"; char c = '\\'';\n", 0},
		{"C23", "[[gnu::weak]] void f(void);\n", 0},
		{"mixed allowed and forbidden", "int x __attribute__((interrupt, weak));\n", 1},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := len(scan(tc.source)); got != tc.want {
				t.Fatalf("scan count = %d, want %d; findings=%+v", got, tc.want, scan(tc.source))
			}
		})
	}
}

func TestLineNumbersAndNestedArguments(t *testing.T) {
	got := scan("int x;\nvoid f(void) __attribute__((format(printf, 1, 2)));\n")
	if len(got) != 1 || got[0].line != 2 {
		t.Fatalf("unexpected findings: %+v", got)
	}
}

func TestInScope(t *testing.T) {
	yes := []string{"libs/a.c", "tests/a.hpp", "examples/builders/a.c", "libs/x/build/a.c", "port/threadx/a.c", "tools/vela/generated/a.c"}
	no := []string{"scripts/a.c", "src/a.c", "libs/third_party/a.c", "apps/shared_libs/third_party/a.c", "libs/ra8_fonts/a.c", "port/x/build_source/a.cpp", "tools/x/CMakeFiles/a.c", "libs/a.cc"}
	for _, p := range yes {
		if !inScope(p) {
			t.Errorf("inScope(%q) = false, want true", p)
		}
	}
	for _, p := range no {
		if inScope(p) {
			t.Errorf("inScope(%q) = true, want false", p)
		}
	}
}

func TestRunExplicitAndFloorFailures(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "libs"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "libs", "bad.c"), []byte("int x __attribute__((weak));\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	var out, errOut bytes.Buffer
	if code := Run(context.Background(), root, []string{"libs/bad.c"}, &out, &errOut); code != 1 {
		t.Fatalf("finding exit = %d, want 1; stdout=%s stderr=%s", code, out.String(), errOut.String())
	}
	out.Reset()
	errOut.Reset()
	if code := Run(context.Background(), root, []string{"libs/missing.c"}, &out, &errOut); code != 2 {
		t.Fatalf("read failure exit = %d, want 2; stderr=%s", code, errOut.String())
	}
	out.Reset()
	errOut.Reset()
	if code := Run(context.Background(), root, nil, &out, &errOut); code != 2 {
		t.Fatalf("below-floor exit = %d, want 2; stderr=%s", code, errOut.String())
	}
}

func TestSelfTest(t *testing.T) {
	var out, errOut bytes.Buffer
	if code := Run(context.Background(), ".", []string{"--selftest"}, &out, &errOut); code != 0 {
		t.Fatalf("selftest exit = %d; stdout=%s stderr=%s", code, out.String(), errOut.String())
	}
}

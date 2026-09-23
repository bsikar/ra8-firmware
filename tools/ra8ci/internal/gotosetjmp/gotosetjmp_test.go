// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gotosetjmp

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestScanTextFindsTokensAndIgnoresNonCode(t *testing.T) {
	source := "const char *marker = \"/*\";\r\n" +
		"/* goto\r\n" +
		"longjmp\r\n" +
		"*/ const char *text = \"setjmp // goto\";\r\n" +
		"int value = 1; goto done; // setjmp longjmp\r\n"
	got := scanText(source)
	if len(got) != 1 || got[0].token != "goto" || got[0].line != 5 || got[0].source != "int value = 1; goto done; // setjmp longjmp" {
		t.Fatalf("findings = %+v, want one code-position goto on line 5", got)
	}
}

func TestScanTextHandlesEscapedQuotesAndContinuedStrings(t *testing.T) {
	source := "const char *a = \"quoted \\\" goto\";\n" +
		"const char *b = \"continued \\\n" +
		"setjmp\";\n" +
		"longjmp(buf, 1);\n"
	got := scanText(source)
	if len(got) != 1 || got[0].token != "longjmp" || got[0].line != 4 {
		t.Fatalf("findings = %+v, want one longjmp on line 4", got)
	}
}

func TestScopedPathMatchesFirstPartyAndOutputContract(t *testing.T) {
	for _, test := range []struct {
		path string
		want bool
	}{
		{"libs/ra8_core/src/core.c", true},
		{"apps/product/src/main.cpp", true},
		{"port/threadx/common/src/tx.c", true},
		{"tools/emulator/src/main.cpp", true},
		{"tests/unit/test.cpp", false},
		{"docs/generated.c", false},
		{"libs/third_party/miniz/miniz.c", false},
		{"libs/ra8_fonts/table.c", false},
		{"tools/runner/build/CMakeFiles/main.c", false},
		{"tools/project/cmake-build-debug/main.cpp", false},
		{"tools/builders/build_app.c", true},
		{"tools/ra8ci/../main.c", false},
	} {
		if got := isScopedPath(test.path); got != test.want {
			t.Errorf("isScopedPath(%q) = %t, want %t", test.path, got, test.want)
		}
	}
}

func TestRunScansUntrackedSourcesAndSkipsExemptTrees(t *testing.T) {
	root := t.TempDir()
	if err := exec.Command("git", "init", "--quiet", root).Run(); err != nil {
		t.Fatalf("git init: %v", err)
	}
	files := map[string]string{
		"libs/finding.c":            "void f(void) { goto bad; }\n",
		"tools/clean.cpp":           "void clean(void) {}\n",
		"tests/exempt.c":            "void f(void) { longjmp(buf, 1); }\n",
		"libs/third_party/vendor.c": "void f(void) { setjmp(buf); }\n",
		"tools/project/build/out.c": "void f(void) { goto ignored; }\n",
	}
	for name, body := range files {
		file := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(file), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(file, []byte(body), 0600); err != nil {
			t.Fatal(err)
		}
	}
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), root, nil, &stdout, &stderr); code != 1 ||
		!strings.Contains(stderr.String(), "libs/finding.c:1: `goto` is banned") ||
		strings.Contains(stderr.String(), "tests/exempt.c") ||
		strings.Contains(stderr.String(), "third_party/vendor.c") ||
		strings.Contains(stderr.String(), "tools/project/build/out.c") ||
		!strings.Contains(stderr.String(), "1 banned control-flow token(s)") {
		t.Fatalf("scan code=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

func TestRunSelftestAndArgumentValidation(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr); code != 0 ||
		stderr.Len() != 0 || !strings.Contains(stdout.String(), "all cases pass") {
		t.Fatalf("selftest code=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	stdout.Reset()
	stderr.Reset()
	if code := Run(context.Background(), t.TempDir(), []string{"--all"}, &stdout, &stderr); code != 2 {
		t.Fatalf("unreviewed argument code=%d stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

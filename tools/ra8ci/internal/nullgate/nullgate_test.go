// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nullgate

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"
)

func TestFindViolationsMatchesNullPolicy(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	file := filepath.Join(dir, "fixture.c")
	content := "int a = NULL; // NULL in comments is ignored\n" +
		"/* NULL in block comment is ignored */ int b = NULL;\n" +
		"const char *s = \"NULL\"; int c = UX_NULL;\n" +
		"/* a block comment\nNULL\n*/ int d = nullptr;\n" +
		"int e = TX_NULL; int f = NULLIFY; int g = NULL;\n"
	if err := os.WriteFile(file, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	got := findViolations(file)
	if len(got) != 3 {
		t.Fatalf("findViolations() returned %d findings, want 3: %#v", len(got), got)
	}
	if got[0].line != 1 || got[1].line != 2 || got[2].line != 7 {
		t.Fatalf("finding lines = %#v, want 1, 2, 7", got)
	}
}

func TestInScopePolicy(t *testing.T) {
	t.Parallel()
	cases := []struct {
		path string
		want bool
	}{
		{"tools/mkbookimg/src/mkbookimg.c", true},
		{"tests/test_x.c", false},
		{"src/main.C", false},
		{"libs/ra8_c6link/src/ra8_media_download.pb-c.c", false},
		{"libs/ra8_c6link/src/future_generated.pb-c.c", true},
		{"libs/third_party/threadx/src/tx.c", false},
		{"apps/shared_libs/third_party/miniz/miniz.c", false},
		{"apps/shared_libs/compress/src/compress.c", true},
		{"libs/x/build/out.c", true},
		{"tools/foo/build/out.c", false},
	}
	for _, tc := range cases {
		if got := inScope(tc.path); got != tc.want {
			t.Errorf("inScope(%q) = %v, want %v", tc.path, got, tc.want)
		}
	}
}

func TestRunExplicitFileOutput(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "src"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "src", "bad.c"), []byte("int x = NULL;\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, []string{"src/bad.c"}, &stdout, &stderr)
	if code != 1 {
		t.Fatalf("Run exit = %d, want 1", code)
	}
	if stdout.Len() != 0 {
		t.Fatalf("unexpected stdout: %q", stdout.String())
	}
	want := "src/bad.c:1: bare NULL -- use nullptr (C23): int x = NULL;\n\n1 bare NULL token(s) found. Replace with `nullptr` (C23 builtin). Allowed: UX_NULL / TX_NULL / FX_NULL / NX_NULL vendor macros, comments, string literals.\n"
	if stderr.String() != want {
		t.Fatalf("stderr = %q, want %q", stderr.String(), want)
	}
}

func TestDecodeReplaceMatchesPythonUTF8Replace(t *testing.T) {
	t.Parallel()
	cases := []struct {
		input []byte
		want  string
	}{
		{[]byte{0xff, 0xff}, "��"},
		{[]byte{0xe2, 0x82}, "�"},
		{[]byte{0xe2, 0x82, 0x41}, "�A"},
		{[]byte{0xe2, 0x28, 0xa1}, "�(�"},
		{[]byte{0xe0, 0x80, 0x80}, "���"},
		{[]byte{0xed, 0xa0, 0x80}, "���"},
		{[]byte{0xf4, 0x90, 0x80, 0x80}, "����"},
	}
	for _, tc := range cases {
		if got := decodeReplace(tc.input); got != tc.want {
			t.Errorf("decodeReplace(%v) = %q, want %q", tc.input, got, tc.want)
		}
	}
}

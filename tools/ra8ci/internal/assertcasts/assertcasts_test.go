// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package assertcasts

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestScanBehavior(t *testing.T) {
	src := "// TEST_ASSERT_EQ((int)a, b)\nconst char *s = \"TEST_ASSERT_EQ(a, (size_t)b)\";\nTEST_ASSERT_EQ(load((int)a), b);\nTEST_ASSERT_EQ((uint64_t)a, fn(x, y));\n"
	got := scan(src, "x.c")
	if len(got) != 3 || !strings.Contains(got[0], "x.c:1: cast in first arg") || !strings.Contains(got[1], "x.c:2: cast in second arg") || !strings.Contains(got[2], "x.c:4: cast in first arg") {
		t.Fatalf("diagnostics = %v", got)
	}
}

func TestNestedCommaAndMalformedCall(t *testing.T) {
	got := scan("TEST_ASSERT_EQ(fn(a, b), (int)e);\nTEST_ASSERT_EQ((int)(a[1, 2]), v);\nTEST_ASSERT_EQ((int)x);\n", "x.c")
	if len(got) != 2 || !strings.Contains(got[0], "second arg") || !strings.Contains(got[1], "first arg") {
		t.Fatalf("diagnostics = %v", got)
	}
}

func TestRunExplicitAllAndUnreadable(t *testing.T) {
	root := t.TempDir()
	p := filepath.Join(root, "tests", "nested", "bad.c")
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte("TEST_ASSERT_EQ((int)x, y);\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	var out, stderr bytes.Buffer
	if code := Run(context.Background(), root, []string{"tests/nested/bad.c"}, &out, &stderr); code != 1 || !strings.Contains(out.String(), "tests/nested/bad.c:1:") {
		t.Fatalf("explicit: code=%d out=%q err=%q", code, out.String(), stderr.String())
	}
	out.Reset()
	stderr.Reset()
	if code := Run(context.Background(), root, []string{"--all"}, &out, &stderr); code != 1 || !strings.Contains(out.String(), p+":1:") {
		t.Fatalf("all: code=%d out=%q err=%q", code, out.String(), stderr.String())
	}
	out.Reset()
	stderr.Reset()
	if code := Run(context.Background(), root, []string{"absent.c"}, &out, &stderr); code != 2 {
		t.Fatalf("missing code=%d", code)
	}
}

func TestSelfTest(t *testing.T) {
	var out, stderr bytes.Buffer
	if code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &out, &stderr); code != 0 {
		t.Fatalf("code=%d out=%q err=%q", code, out.String(), stderr.String())
	}
}

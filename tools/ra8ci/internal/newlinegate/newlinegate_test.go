// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRunExplicitFilesAndEmptyFile(t *testing.T) {
	root := t.TempDir()
	good := filepath.Join(root, "good.py")
	bad := filepath.Join(root, "bad.py")
	empty := filepath.Join(root, "empty.py")
	for path, data := range map[string][]byte{
		good:  []byte("ok\n"),
		bad:   []byte("missing"),
		empty: nil,
	} {
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), root, []string{"good.py", "bad.py", "empty.py"}, &stdout, &stderr); code != 1 {
		t.Fatalf("scan code = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	if !strings.Contains(stderr.String(), "bad.py") || strings.Contains(stderr.String(), "good.py") {
		t.Fatalf("unexpected findings: %q", stderr.String())
	}
	if err := os.WriteFile(bad, []byte("fixed\n"), 0600); err != nil {
		t.Fatal(err)
	}
	stdout.Reset()
	stderr.Reset()
	if code := Run(context.Background(), root, []string{"good.py", "bad.py", "empty.py"}, &stdout, &stderr); code != 0 {
		t.Fatalf("clean scan code = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

func TestDirectoryScanUsesOnlyFirstPartySources(t *testing.T) {
	root := t.TempDir()
	for path, data := range map[string][]byte{
		filepath.Join(root, "tools", "source.py"):                 []byte("bad"),
		filepath.Join(root, "tools", "justfile"):                  []byte("bad"),
		filepath.Join(root, "libs", "third_party", "vendor.py"):   []byte("bad"),
		filepath.Join(root, "tools", "foo", "build", "output.py"): []byte("bad"),
	} {
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), root, []string{"tools", "libs"}, &stdout, &stderr); code != 1 {
		t.Fatalf("directory scan code = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	if !strings.Contains(stderr.String(), "source.py") || !strings.Contains(stderr.String(), "justfile") {
		t.Fatalf("first-party findings missing: %q", stderr.String())
	}
	if strings.Contains(stderr.String(), "vendor.py") || strings.Contains(stderr.String(), "output.py") {
		t.Fatalf("excluded files were scanned: %q", stderr.String())
	}
}

func TestSelfTestLiveScope(t *testing.T) {
	root := filepath.Clean(filepath.Join("..", "..", "..", ".."))
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), root, []string{"--selftest"}, &stdout, &stderr); code != 0 {
		t.Fatalf("selftest code = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

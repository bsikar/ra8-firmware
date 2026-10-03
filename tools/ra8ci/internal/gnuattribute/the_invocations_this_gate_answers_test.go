// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gnuattribute

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// This gate reads first-party C and C++ and reports GNU attributes that
// should be C23 syntax. Everything below is what it decides before it
// reads a line: which invocations it answers, which paths are its
// business, and what it does with a tree it cannot walk.

// planted writes the files into a fresh root and hands the root back.
func planted(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for name, body := range files {
		full := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

// ran is one invocation: exit code and both streams.
type ran struct {
	code   int
	stdout string
	stderr string
}

func gate(t *testing.T, ctx context.Context, root string, args ...string) ran {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, args, &stdout, &stderr)
	return ran{code: code, stdout: stdout.String(), stderr: stderr.String()}
}

// An option this gate does not have is refused with its usage rather than
// taken for a filename, which is how "--fix" would otherwise be read as a
// path and reported as unreadable.
func TestAnOptionThisGateDoesNotHaveIsRefusedWithItsUsage(t *testing.T) {
	root := planted(t, map[string]string{"libs/a.c": "int x;\n"})
	for _, args := range [][]string{
		{"--fix"}, {"-v"}, {"--selftest", "libs/a.c"}, {"libs/a.c", "--quiet"},
	} {
		got := gate(t, context.Background(), root, args...)
		if got.code != 2 || !strings.Contains(got.stderr, "usage: ra8ci gnu-attribute") {
			t.Fatalf("%v = %+v, want the usage line", args, got)
		}
		if got.stdout != "" {
			t.Fatalf("%v reported %q", args, got.stdout)
		}
	}
}

// Explicit paths narrow the scan, and a clean narrow scan says how many
// files it actually read, so an operator can see the scope was real.
func TestACleanScanSaysHowManyFilesItRead(t *testing.T) {
	root := planted(t, map[string]string{
		"libs/ok.c":            "[[gnu::weak]] void f(void);\n",
		"libs/irq.c":           "void irq(void) __attribute__((interrupt));\n",
		"docs/notes.md":        "__attribute__((weak))\n",
		"libs/third_party/v.c": "int x __attribute__((packed));\n",
		"libs/ra8_fonts/big.c": "int y __attribute__((packed));\n",
	})

	got := gate(t, context.Background(), root, "libs/ok.c", "libs/irq.c",
		"docs/notes.md", "libs/third_party/v.c", "libs/ra8_fonts/big.c")
	if got.code != 0 || got.stderr != "" {
		t.Fatalf("clean scan = %+v", got)
	}
	if !strings.Contains(got.stdout, "clean -- 2 file(s) scanned.") {
		t.Fatalf("clean scan reported %q, want only the two in-scope files read", got.stdout)
	}
}

// A caller that walks away stops the scan rather than reading the rest of
// the list for nobody.
func TestAScanStopsWhenTheCallerWalksAway(t *testing.T) {
	root := planted(t, map[string]string{"libs/a.c": "int x;\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	got := gate(t, ctx, root, "libs/a.c")
	if got.code != 2 || !strings.Contains(got.stderr, "cancelled") {
		t.Fatalf("cancelled scan = %+v", got)
	}
	if got.stdout != "" {
		t.Fatalf("a cancelled scan reported %q", got.stdout)
	}
}

// A whole-tree sweep that collapses is fatal rather than clean, and the
// floor is what protects CI from a scan that found almost nothing.
func TestAWholeTreeSweepThatCollapsesIsFatal(t *testing.T) {
	root := planted(t, map[string]string{"libs/a.c": "int x;\n"})

	got := gate(t, context.Background(), root)
	if got.code != 2 || !strings.Contains(got.stderr, "collapsed sweep is not trustworthy") {
		t.Fatalf("collapsed sweep = %+v", got)
	}
	if strings.Contains(got.stdout, "clean") {
		t.Fatalf("a collapsed sweep reported clean: %q", got.stdout)
	}
}

// A source root that is a file rather than a directory is passed over.
// The repository really does carry a "tools" script in some checkouts,
// and a gate that faulted on it would block every build that had one.
func TestASourceRootThatIsAFileIsPassedOver(t *testing.T) {
	root := planted(t, map[string]string{"tools": "#!/bin/sh\n", "libs/a.c": "int x;\n"})

	files, err := discover(root)
	if err != nil {
		t.Fatalf("discovery faulted on a file named like a root: %v", err)
	}
	if len(files) != 1 || files[0] != "libs/a.c" {
		t.Fatalf("discovered %v", files)
	}
}

// A path that leaves the tree, or that is not already the clean relative
// form the walk produces, is not this gate's business.
func TestAPathThatLeavesTheTreeIsNotInScope(t *testing.T) {
	for _, rel := range []string{
		"", "/libs/a.c", "../libs/a.c", "libs/./a.c", "libs/../libs/a.c",
		"libs/a.c/", "./libs/a.c",
	} {
		if inScope(rel) {
			t.Fatalf("%q was taken, want it refused", rel)
		}
	}
	for _, rel := range []string{"libs/a.c", "port/threadx/p.h", "apps/ui/main.cpp"} {
		if !inScope(rel) {
			t.Fatalf("%q was refused, want it taken", rel)
		}
	}
}

// An attribute whose argument list never closes is not a finding. Half a
// macro mid-edit, or a truncated generated header, must not be read as an
// attribute whose body happens to run to the end of the file.
func TestAnAttributeThatNeverClosesIsNotABody(t *testing.T) {
	for name, source := range map[string]string{
		"no parentheses at all": "__attribute__ weak;",
		"one parenthesis":       "__attribute__ (weak);",
		"never closed":          "void f(void) __attribute__((weak;",
		"closed once only":      "void f(void) __attribute__((visibility(\"default\");",
	} {
		if body, ok := attrBody(source, 0); ok {
			t.Fatalf("%s: read %q as an attribute body", name, body)
		}
	}
	body, ok := attrBody("void f(void) __attribute__((visibility(\"default\"))) ;", 0)
	if !ok || body != `visibility("default")` {
		t.Fatalf("a nested body read as %q, %v", body, ok)
	}
}

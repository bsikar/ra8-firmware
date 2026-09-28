// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package legacymake

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// plantFullScope plants a repository whose derived scope clears the file
// floor, plus whatever the case itself wants in it. The filler is prose, which
// the scope keeps by suffix and the scanner reads as prose rather than as
// commands.
func plantFullScope(t *testing.T, extra map[string]string) string {
	t.Helper()
	files := map[string]string{gateSource: "package legacymake\n"}
	for i := 0; i < minimumScopedFiles; i++ {
		files[fmt.Sprintf("docs/notes/unit%04d.md", i)] = "prose\n"
	}
	for rel, contents := range extra {
		files[rel] = contents
	}
	return plantRepo(t, files)
}

// ranGate runs the gate over root and answers its exit status and both streams.
func ranGate(t *testing.T, root string, args ...string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, args, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestRunReportsACleanScopeAndCountsIt(t *testing.T) {
	root := plantFullScope(t, nil)
	code, stdout, stderr := ranGate(t, root)
	if code != 0 {
		t.Fatalf("exit = %d, want 0; stderr = %q", code, stderr)
	}
	want := fmt.Sprintf("ra8ci legacy-make: clean (%d authored files)\n", minimumScopedFiles+1)
	if stdout != want {
		t.Fatalf("stdout = %q, want %q", stdout, want)
	}
	if stderr != "" {
		t.Fatalf("clean run wrote to stderr: %q", stderr)
	}
}

// The gate's whole judgement is that the same eight characters are a command
// in a script and a mention in prose. A run that reported both, or neither,
// would be useless to the author it is meant to stop.
func TestRunNamesCommandsInScriptsAndLeavesTheSameWordsInProse(t *testing.T) {
	root := plantFullScope(t, map[string]string{
		"scripts/build.sh": "#!/bin/sh\nmake ci\n",
		"docs/plain.md":    "make ci\n",
	})
	code, stdout, stderr := ranGate(t, root)
	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a failing run wrote to stdout: %q", stdout)
	}
	if !strings.Contains(stderr, "  scripts/build.sh:2: legacy repository task: make ci\n") {
		t.Fatalf("stderr does not name the shell invocation: %q", stderr)
	}
	if strings.Contains(stderr, "docs/plain.md") {
		t.Fatalf("stderr reports a bare mention in prose: %q", stderr)
	}
	if !strings.Contains(stderr, "Use the authoritative namespaced Just recipe instead.") {
		t.Fatalf("stderr omits the remedy: %q", stderr)
	}
}

// Prose is not exempt: what the gate refuses there is a line telling a reader
// to run the thing.
func TestRunReadsProseThatInstructsAReaderToRunIt(t *testing.T) {
	root := plantFullScope(t, map[string]string{
		"docs/guide.md": "Setup notes\nPlease run make misra\n",
		"docs/head.md":  "# make ci-native\n",
	})
	code, _, stderr := ranGate(t, root)
	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	for _, want := range []string{
		"  docs/guide.md:2: legacy repository task: make misra\n",
		"  docs/head.md:1: legacy repository task: make ci-native\n",
	} {
		if !strings.Contains(stderr, want) {
			t.Fatalf("stderr omits %q: %q", want, stderr)
		}
	}
}

func TestRunListsFindingsByPathThenLine(t *testing.T) {
	root := plantFullScope(t, map[string]string{
		"scripts/b.sh": "#!/bin/sh\nmake ci\n",
		"scripts/a.sh": "#!/bin/sh\nsleep 1\nmake sbom\nmake misra\n",
	})
	code, _, stderr := ranGate(t, root)
	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	order := []string{"scripts/a.sh:3", "scripts/a.sh:4", "scripts/b.sh:2"}
	at := -1
	for _, want := range order {
		index := strings.Index(stderr, want)
		if index < 0 {
			t.Fatalf("stderr omits %s: %q", want, stderr)
		}
		if index < at {
			t.Fatalf("%s is out of order in %q", want, stderr)
		}
		at = index
	}
}

// A file the gate cannot read is refused, not skipped: a gate that passed over
// what it failed to open would report a tree clean it never scanned.
func TestRunRefusesAFileItCannotRead(t *testing.T) {
	root := plantFullScope(t, map[string]string{"docs/sealed.md": "prose\n"})
	sealed := filepath.Join(root, "docs", "sealed.md")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatalf("chmod: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o644) })
	if _, err := os.ReadFile(sealed); err == nil {
		t.Skip("this box reads a mode 0o000 file")
	}
	code, stdout, stderr := ranGate(t, root)
	if code != 2 {
		t.Fatalf("exit = %d, want 2; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a refused run wrote to stdout: %q", stdout)
	}
	if !strings.Contains(stderr, "scan failed") || !strings.Contains(stderr, "docs/sealed.md") {
		t.Fatalf("stderr does not name the unreadable file: %q", stderr)
	}
}

// Undecodable bytes are passed over rather than refused: a compiled artefact
// that slipped into the scope is not an authoring mistake, and the line
// numbers of a file that is not text mean nothing.
func TestScanPassesOverUndecodableBytesAndReadsTheSameTextWhenValid(t *testing.T) {
	line := "Please run make misra\n"
	root := plantTree(t, map[string]string{
		"docs/binary.md": "\xff\xfe" + line,
		"docs/text.md":   line,
	})
	found, err := scan(context.Background(), root, []string{"docs/binary.md", "docs/text.md"})
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	if len(found) != 1 {
		t.Fatalf("findings = %+v, want only the decodable file", found)
	}
	if found[0].path != "docs/text.md" || found[0].line != 1 || found[0].command != "make misra" {
		t.Fatalf("finding = %+v", found[0])
	}
}

func TestScanRefusesACancelledRun(t *testing.T) {
	root := plantTree(t, map[string]string{"docs/guide.md": "Please run make misra\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	found, err := scan(ctx, root, []string{"docs/guide.md"})
	if err == nil {
		t.Fatalf("a cancelled scan answered findings %+v", found)
	}
	if !strings.Contains(err.Error(), "context canceled") {
		t.Fatalf("error = %v, want the cancellation", err)
	}
	if found != nil {
		t.Fatalf("a cancelled scan answered partial findings: %+v", found)
	}
}

// The gate's own source is in scope by name, so its detector examples must not
// be read as findings against it.
func TestRunDoesNotReportTheGateSourceAgainstItself(t *testing.T) {
	root := plantFullScope(t, map[string]string{
		gateSource: "package legacymake\n// Please run make misra\n",
	})
	code, _, stderr := ranGate(t, root)
	if code != 1 {
		t.Skipf("the gate source planted here is not read as a finding; exit %d", code)
	}
	if !strings.Contains(stderr, gateSource) {
		t.Fatalf("stderr does not name the gate source: %q", stderr)
	}
}

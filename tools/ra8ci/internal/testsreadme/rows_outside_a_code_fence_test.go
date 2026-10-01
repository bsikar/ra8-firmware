// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"context"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// documented reads a README the way the gate does and returns the row names in
// a stable order, so a test can state the whole expected set.
func documented(readme string) []string {
	names := make([]string, 0, 4)
	for name := range documentedSubdirs(readme) {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// table renders a README whose real table holds the named rows, with body text
// placed verbatim between the heading and the table.
func table(body string, rows ...string) string {
	var built strings.Builder
	built.WriteString("# tests/\n\n")
	built.WriteString(body)
	built.WriteString("\n| Subdirectory | What it holds |\n|---|---|\n")
	for _, name := range rows {
		built.WriteString("| `" + name + "/` | fixture description |\n")
	}
	return built.String()
}

// fixtureTree writes a tests tree and its README, and returns both paths in the
// order evaluate takes them.
func fixtureTree(t *testing.T, readme string, subdirs ...string) (string, string) {
	t.Helper()
	testsDir := filepath.Join(t.TempDir(), "tests")
	if err := os.Mkdir(testsDir, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range subdirs {
		if err := os.Mkdir(filepath.Join(testsDir, name), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	path := filepath.Join(testsDir, "README.md")
	if err := os.WriteFile(path, []byte(readme), 0o644); err != nil {
		t.Fatal(err)
	}
	return testsDir, path
}

func TestARowInsideAFenceIsNotDocumentation(t *testing.T) {
	readme := table("Add a row like this one:\n\n```\n| `example/` | what it holds |\n```\n", "core", "hal")
	if got := documented(readme); strings.Join(got, ",") != "core,hal" {
		t.Fatalf("documented = %v, want core and hal only", got)
	}
}

func TestARowAfterAClosedFenceIsDocumentation(t *testing.T) {
	readme := "```\n| `example/` | shown, not claimed |\n```\n\n| `core/` | real |\n"
	if got := documented(readme); strings.Join(got, ",") != "core" {
		t.Fatalf("documented = %v, want core", got)
	}
}

func TestAnUnclosedFenceSwallowsTheRestOfTheFile(t *testing.T) {
	readme := "| `core/` | real |\n```\n| `example/` | never closed |\n| `other/` | still inside |\n"
	if got := documented(readme); strings.Join(got, ",") != "core" {
		t.Fatalf("documented = %v, want core", got)
	}
}

func TestATildeFenceHidesRowsToo(t *testing.T) {
	readme := "~~~markdown\n| `example/` | shown |\n~~~\n| `core/` | real |\n"
	if got := documented(readme); strings.Join(got, ",") != "core" {
		t.Fatalf("documented = %v, want core", got)
	}
}

func TestAFenceClosesOnlyOnItsOwnCharacter(t *testing.T) {
	readme := "```\n| `example/` | inside |\n~~~\n| `other/` | still inside |\n```\n| `core/` | real |\n"
	if got := documented(readme); strings.Join(got, ",") != "core" {
		t.Fatalf("documented = %v, want core", got)
	}
}

func TestAShorterRunDoesNotCloseALongerFence(t *testing.T) {
	readme := "````\n| `example/` | inside |\n```\n| `other/` | still inside |\n````\n| `core/` | real |\n"
	if got := documented(readme); strings.Join(got, ",") != "core" {
		t.Fatalf("documented = %v, want core", got)
	}
}

func TestAnInfoStringOpensButNeverCloses(t *testing.T) {
	readme := "```\n| `example/` | inside |\n``` text\n| `other/` | still inside |\n```\n| `core/` | real |\n"
	if got := documented(readme); strings.Join(got, ",") != "core" {
		t.Fatalf("documented = %v, want core", got)
	}
}

func TestAFenceIndentedPastThreeSpacesIsNotAFence(t *testing.T) {
	readme := "    ```\n| `core/` | still read as a row |\n    ```\n"
	if got := documented(readme); strings.Join(got, ",") != "core" {
		t.Fatalf("documented = %v, want core", got)
	}
}

func TestFenceMarkerReportsTheRunAndWhatFollows(t *testing.T) {
	for _, test := range []struct {
		line string
		char byte
		run  int
		rest string
	}{
		{line: "```", char: '`', run: 3},
		{line: "  ````go", char: '`', run: 4, rest: "go"},
		{line: "~~~~", char: '~', run: 4},
		{line: "``", run: 0},
		{line: "| `core/` | a row |", run: 0},
		{line: "     ```", run: 0},
	} {
		char, run, rest := fenceMarker(test.line)
		if run != test.run || rest != test.rest || (run != 0 && char != test.char) {
			t.Errorf("fenceMarker(%q) = %q %d %q, want %q %d %q", test.line, char, run, rest, test.char, test.run, test.rest)
		}
	}
}

func TestAFencedExampleDoesNotFailTheGate(t *testing.T) {
	readme := table("Rows look like this:\n\n```\n| `example/` | what it holds |\n```\n", "alpha", "beta", "gamma")
	testsDir, path := fixtureTree(t, readme, "alpha", "beta", "gamma")
	code, messages, _, err := evaluate(context.Background(), testsDir, path, 3, sanitizedGitEnvironment(os.Environ()))
	if err != nil || code != exitOK {
		t.Fatalf("fenced example = exit %d %v (%v), want a quiet gate", code, messages, err)
	}
}

func TestAFencedExampleDoesNotDocumentARealSubdirectory(t *testing.T) {
	readme := table("The gamma row was deleted; only the example still names it:\n\n```\n| `gamma/` | what it holds |\n```\n", "alpha", "beta")
	testsDir, path := fixtureTree(t, readme, "alpha", "beta", "gamma")
	code, messages, _, err := evaluate(context.Background(), testsDir, path, 3, sanitizedGitEnvironment(os.Environ()))
	if err != nil || code != exitDrift || !containsMessage(messages, "tests/gamma/") {
		t.Fatalf("deleted row behind a fenced example = exit %d %v (%v), want drift naming tests/gamma/", code, messages, err)
	}
}

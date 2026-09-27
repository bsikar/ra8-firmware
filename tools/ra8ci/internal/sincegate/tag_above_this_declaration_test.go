// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package sincegate

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// header writes text to a public header path the presence check recognises and
// returns the path.
func header(t *testing.T, dir, name, text string) string {
	t.Helper()
	path := filepath.Join(dir, "libs", "ra8_thing", "inc", name)
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0600); err != nil {
		t.Fatal(err)
	}
	return path
}

// declared returns a public declaration line for name.
func declared(name string) string {
	return "ra8_err_t " + name + "(void);"
}

func TestATagWrittenForTheDeclarationAboveDoesNotCoverThisOne(t *testing.T) {
	path := header(t, t.TempDir(), "pair.h", strings.Join([]string{
		"/** @brief first. @since 1.2.3 */",
		declared("ra8_documented"),
		"",
		declared("ra8_bare"),
	}, "\n"))
	got := checkPresence(path)
	if len(got) != 1 {
		t.Fatalf("findings = %v, want the bare declaration flagged", got)
	}
	if !strings.Contains(got[0], "ra8_bare") {
		t.Fatalf("finding names the wrong declaration: %s", got[0])
	}
}

func TestEachDeclarationWithItsOwnTagPasses(t *testing.T) {
	path := header(t, t.TempDir(), "both.h", strings.Join([]string{
		"/** @since 1.2.3 */",
		declared("ra8_first"),
		"",
		"/** @since 1.2.3 */",
		declared("ra8_second"),
	}, "\n"))
	if got := checkPresence(path); len(got) != 0 {
		t.Fatalf("documented pair flagged: %v", got)
	}
}

func TestEveryBareDeclarationAfterOneDocumentedBlockIsFlagged(t *testing.T) {
	path := header(t, t.TempDir(), "run.h", strings.Join([]string{
		"/** @since 1.2.3 */",
		declared("ra8_first"),
		declared("ra8_second"),
		declared("ra8_third"),
	}, "\n"))
	got := checkPresence(path)
	if len(got) != 2 {
		t.Fatalf("findings = %v, want both trailing declarations flagged", got)
	}
}

func TestTheWindowStillReachesThirtyLinesWhenNothingInterrupts(t *testing.T) {
	lines := []string{"/** @since 1.2.3 */"}
	for len(lines) < presenceLookback {
		lines = append(lines, "/* filler */")
	}
	lines = append(lines, declared("ra8_far"))
	path := header(t, t.TempDir(), "far.h", strings.Join(lines, "\n"))
	if got := checkPresence(path); len(got) != 0 {
		t.Fatalf("tag at the %d-line boundary rejected: %v", presenceLookback, got)
	}
}

func TestTheWindowStopsAtAnEarlierDeclarationWellInsideThirtyLines(t *testing.T) {
	lines := []string{"/** @since 1.2.3 */", declared("ra8_first")}
	for len(lines) < 10 {
		lines = append(lines, "/* filler */")
	}
	lines = append(lines, declared("ra8_second"))
	path := header(t, t.TempDir(), "inside.h", strings.Join(lines, "\n"))
	got := checkPresence(path)
	if len(got) != 1 || !strings.Contains(got[0], "ra8_second") {
		t.Fatalf("findings = %v, want only ra8_second flagged", got)
	}
}

func TestCommentsAndCodeThatAreNotDeclarationsDoNotStopTheWindow(t *testing.T) {
	path := header(t, t.TempDir(), "noise.h", strings.Join([]string{
		"/** @since 1.2.3 */",
		"#define RA8_THING 1",
		"typedef struct ra8_thing ra8_thing_t;",
		"/* still the same block's neighbourhood */",
		declared("ra8_after_noise"),
	}, "\n"))
	if got := checkPresence(path); len(got) != 0 {
		t.Fatalf("non-declaration lines cut the window: %v", got)
	}
}

func TestADeclarationOnTheFirstLineHasNothingToReadAndIsFlagged(t *testing.T) {
	path := header(t, t.TempDir(), "top.h", declared("ra8_first_line")+"\n")
	if got := checkPresence(path); len(got) != 1 {
		t.Fatalf("findings = %v, want the first-line declaration flagged", got)
	}
}

func TestCommentAboveReturnsOnlyTheLinesBelowTheEarlierDeclaration(t *testing.T) {
	lines := []string{
		"/** @since 1.2.3 */",
		declared("ra8_first"),
		"/** @since 1.2.3 */",
		declared("ra8_second"),
	}
	got := commentAbove(lines, 3)
	if len(got) != 1 || got[0] != lines[2] {
		t.Fatalf("commentAbove = %q, want just the second block", got)
	}
	if first := commentAbove(lines, 1); len(first) != 1 || first[0] != lines[0] {
		t.Fatalf("commentAbove for the first declaration = %q", first)
	}
	if none := commentAbove(lines, 0); len(none) != 0 {
		t.Fatalf("commentAbove at index 0 = %q, want empty", none)
	}
}

func TestCommentAboveNeverReadsPastTheStartOfTheFile(t *testing.T) {
	lines := []string{"/* a */", "/* b */", declared("ra8_near_top")}
	if got := commentAbove(lines, 2); len(got) != 2 {
		t.Fatalf("commentAbove near the top = %q", got)
	}
}

func TestRunReportsTheInheritedTagAsAFinding(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "VERSION"), []byte("1.2.3\n"), 0600); err != nil {
		t.Fatal(err)
	}
	path := header(t, root, "public.h", strings.Join([]string{
		"/** @since 1.2.3 */",
		declared("ra8_documented"),
		declared("ra8_bare"),
	}, "\n"))
	var out, errOut bytes.Buffer
	if code := Run(context.Background(), root, []string{path}, &out, &errOut); code != 1 {
		t.Fatalf("Run code = %d, want 1: %s", code, errOut.String())
	}
	if !strings.Contains(errOut.String(), "ra8_bare missing @since") {
		t.Fatalf("Run output does not name the bare declaration: %s", errOut.String())
	}
	if strings.Contains(errOut.String(), "ra8_documented missing") {
		t.Fatalf("Run flagged the documented declaration: %s", errOut.String())
	}
}

func TestRunStaysCleanWhenEveryDeclarationCarriesItsOwnTag(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "VERSION"), []byte("1.2.3\n"), 0600); err != nil {
		t.Fatal(err)
	}
	path := header(t, root, "clean.h", strings.Join([]string{
		"/** @since 1.2.3 */",
		declared("ra8_first"),
		"",
		"/** @since 1.2.3 */",
		declared("ra8_second"),
	}, "\n"))
	var out, errOut bytes.Buffer
	if code := Run(context.Background(), root, []string{path}, &out, &errOut); code != 0 {
		t.Fatalf("Run code = %d, want 0: %s", code, errOut.String())
	}
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package waverefs

import (
	"context"
	"strings"
	"testing"
)

// The scan path behind the file floor had no test, because the floor is
// 2500 files and that looked expensive. It is not: 2500 empty files cost a
// couple of seconds, and every branch past the floor (clean, findings,
// truncation, the snippet trim) is only reachable through them.
func plantFullScope(t *testing.T, extra map[string]string) string {
	t.Helper()
	files := make(map[string]string, fileFloor+len(extra))
	for index := 0; index < fileFloor; index++ {
		files["apps/unit"+itoa(index)+".md"] = ""
	}
	for rel, contents := range extra {
		files[rel] = contents
	}
	return plantRepo(t, files)
}

func ran(t *testing.T, root string, args ...string) (int, string, string) {
	t.Helper()
	var out, errs strings.Builder
	code := Run(context.Background(), root, args, &out, &errs)
	return code, out.String(), errs.String()
}

// A full scope with nothing to find is the only case that may print "gate
// clean", and it must say so on stdout with nothing on stderr.
func TestAFullScopeWithNothingToFindIsGateClean(t *testing.T) {
	code, out, errs := ran(t, plantFullScope(t, nil))
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "no-wave-refs: 0 violations -- gate clean.") {
		t.Fatalf("stdout = %q, want the clean verdict", out)
	}
	if errs != "" {
		t.Fatalf("stderr = %q, want nothing", errs)
	}
}

// One finding fails the gate, names where it is, and carries the opt-out
// instruction, which is the only thing that tells a reader what to do next.
func TestOneFindingFailsTheGateAndSaysWhereItIs(t *testing.T) {
	root := plantFullScope(t, map[string]string{
		"docs/plan.md": "intro\nthe Wave 3 rollout\ntrailer\n",
	})
	code, out, errs := ran(t, root)
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "no-wave-refs: 1 violations found.") {
		t.Fatalf("stdout = %q, want the count", out)
	}
	if !strings.Contains(out, "docs/plan.md:2 the Wave 3 rollout") {
		t.Fatalf("stdout = %q, want the path, line and text", out)
	}
	if !strings.Contains(out, `Per-line opt-out: append "WAVE-OK: <reason>"`) {
		t.Fatalf("stdout = %q, want the opt-out instruction", out)
	}
	if strings.Contains(out, "gate clean") {
		t.Fatalf("stdout = %q, must not call a failing tree clean", out)
	}
}

// Past maxFindingsShown the list is cut and the remainder is counted, so a
// tree with hundreds of hits does not bury the terminal. The COUNT stays
// whole: only the listing is truncated.
func TestMoreFindingsThanTheGateListsAreCountedAndTruncated(t *testing.T) {
	const planted = maxFindingsShown + 7
	extra := make(map[string]string, planted)
	for index := 0; index < planted; index++ {
		extra["docs/note"+itoa(index)+".md"] = "see Wave 4 for this\n"
	}
	code, out, errs := ran(t, plantFullScope(t, extra))
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "no-wave-refs: "+itoa(planted)+" violations found.") {
		t.Fatalf("stdout = %q, want all %d counted", out, planted)
	}
	if !strings.Contains(out, "... 7 more (truncated)") {
		t.Fatalf("stdout = %q, want the truncation line", out)
	}
	listed := strings.Count(out, "see Wave 4 for this")
	if listed != maxFindingsShown {
		t.Fatalf("listed %d findings, want exactly %d", listed, maxFindingsShown)
	}
}

// Exactly maxFindingsShown findings are all listed and nothing is called
// truncated, which is the other side of the bound.
func TestExactlyTheShownLimitIsListedWhole(t *testing.T) {
	extra := make(map[string]string, maxFindingsShown)
	for index := 0; index < maxFindingsShown; index++ {
		extra["docs/note"+itoa(index)+".md"] = "see Wave 5 for this\n"
	}
	code, out, errs := ran(t, plantFullScope(t, extra))
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	if strings.Contains(out, "(truncated)") {
		t.Fatalf("stdout = %q, must not truncate at the limit itself", out)
	}
	if listed := strings.Count(out, "see Wave 5 for this"); listed != maxFindingsShown {
		t.Fatalf("listed %d findings, want %d", listed, maxFindingsShown)
	}
}

// A very long line is trimmed to a snippet rather than printed whole, and
// the trim is by RUNES, so a line of multi-byte text is cut where a reader
// would expect rather than through a character.
func TestALongFindingIsTrimmedToASnippet(t *testing.T) {
	long := strings.Repeat("e", 200) + " Wave 6"
	root := plantFullScope(t, map[string]string{"docs/long.md": long + "\n"})
	code, out, errs := ran(t, root)
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	if strings.Contains(out, long) {
		t.Fatal("stdout carried the whole long line rather than a snippet")
	}
	if !strings.Contains(out, strings.Repeat("e", snippetTrimLen)+"...") {
		t.Fatalf("stdout = %q, want the line trimmed to %d runes and an ellipsis", out, snippetTrimLen)
	}

	wide := strings.Repeat("\u00e9", 200) + " Wave 7"
	code, out, _ = ran(t, plantFullScope(t, map[string]string{"docs/wide.md": wide + "\n"}))
	if code != 1 {
		t.Fatalf("multi-byte: code = %d, want 1", code)
	}
	if !strings.Contains(out, strings.Repeat("\u00e9", snippetTrimLen)+"...") {
		t.Fatalf("multi-byte line was not trimmed on a rune boundary: %q", out)
	}
}

// A line exactly at the snippet bound is printed whole: the trim is for
// lines PAST it, and an off-by-one here would mangle ordinary text.
func TestALineAtTheSnippetBoundIsPrintedWhole(t *testing.T) {
	exact := "Wave 8 " + strings.Repeat("f", snippetMaxLen-7)
	if len([]rune(exact)) != snippetMaxLen {
		t.Fatalf("fixture is %d runes, want %d", len([]rune(exact)), snippetMaxLen)
	}
	code, out, errs := ran(t, plantFullScope(t, map[string]string{"docs/exact.md": exact + "\n"}))
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, exact) {
		t.Fatalf("stdout = %q, want the line at the bound printed whole", out)
	}
	if strings.Contains(out, "...") {
		t.Fatalf("stdout = %q, must not trim at the bound itself", out)
	}
}

// A scope that cannot be derived at all is a hard failure naming the
// derivation, never a clean tree and never the floor complaint: the two send
// an operator to different places.
func TestRunRefusesAScopeItCannotDerive(t *testing.T) {
	code, out, errs := ran(t, t.TempDir())
	if code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	if !strings.Contains(errs, "cannot derive source scope") {
		t.Fatalf("stderr = %q, want the derivation named", errs)
	}
	if strings.Contains(errs, "floor is") {
		t.Fatalf("stderr = %q, a failed derivation is not a collapsed scope", errs)
	}
	if out != "" {
		t.Fatalf("stdout = %q, want nothing", out)
	}
}

// An opt-out that states its reason keeps a full scope clean, which is the
// whole point of having one: it has to work through Run, not only through
// scan.
func TestAnOptOutWithAReasonKeepsAFullScopeClean(t *testing.T) {
	root := plantFullScope(t, map[string]string{
		"docs/excused.md": "the Wave 9 rollout WAVE-OK: quoting the vendor's own release name\n",
	})
	code, out, errs := ran(t, root)
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(out, "gate clean") {
		t.Fatalf("stdout = %q, want the clean verdict", out)
	}
}

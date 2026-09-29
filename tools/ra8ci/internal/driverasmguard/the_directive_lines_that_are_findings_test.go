// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package driverasmguard

import (
	"strings"
	"testing"
)

// The guard reads a driver line by line and keeps a stack of whether it is
// standing inside a RA8_OFF_TARGET conditional. Most lines are either a
// conditional directive it tracks or ordinary code it scans for asm. A
// third kind exists and is easy to miss: a preprocessor line that is NOT
// one of the five conditional keywords and carries the asm itself, such as
// a #define wrapping an instruction. That line is both a directive and a
// finding, and the guard has to judge it as one rather than skip it for
// starting with a hash.
//
// A macro is the worst place for this to be missed. Bare asm written into
// a #define under RA8_OFF_TARGET reaches the host build at every call site
// rather than one, which is exactly what the shared intrinsics header
// exists to stop.

func findingsIn(t *testing.T, source string) []finding {
	t.Helper()
	return checkSource("libs/ra8_hal/src/ra8_gpio.c", source)
}

// A #define carrying asm inside an off-target conditional is a finding at
// its own line, with the line reported as written and trimmed.
func TestAMacroThatHidesAsmUnderOffTargetIsStillAFinding(t *testing.T) {
	source := strings.Join([]string{
		"#ifdef RA8_OFF_TARGET",
		"  #define RA8_NOP() __asm__(\"nop\")",
		"#endif",
		"",
	}, "\n")

	problems := findingsIn(t, source)
	if len(problems) != 1 {
		t.Fatalf("findings = %d, want the macro line: %#v", len(problems), problems)
	}
	if problems[0].line != 2 {
		t.Fatalf("line = %d, want 2", problems[0].line)
	}
	if problems[0].text != "#define RA8_NOP() __asm__(\"nop\")" {
		t.Fatalf("text = %q, want the line trimmed of its indent", problems[0].text)
	}
}

// The same macro outside any conditional is left alone: the rule is about
// what the host build compiles, not about macros.
func TestTheSameMacroOutsideAConditionalIsLeftAlone(t *testing.T) {
	if problems := findingsIn(t, "#define RA8_NOP() __asm__(\"nop\")\n"); len(problems) != 0 {
		t.Fatalf("findings = %#v, want none outside a conditional", problems)
	}
}

// A conditional on something other than RA8_OFF_TARGET is not this gate's
// business, macro or not.
func TestAMacroUnderAnUnrelatedConditionalIsNotThisGatesBusiness(t *testing.T) {
	source := "#ifdef RA8_DEBUG\n#define RA8_NOP() __asm__(\"nop\")\n#endif\n"
	if problems := findingsIn(t, source); len(problems) != 0 {
		t.Fatalf("findings = %#v, want none under RA8_DEBUG", problems)
	}
}

// Every non-conditional directive is judged the same way, so the rule does
// not turn on which keyword happens to carry the asm.
func TestAnyDirectiveCarryingAsmUnderOffTargetFires(t *testing.T) {
	for _, line := range []string{
		"#define RA8_WFI() __asm__(\"wfi\")",
		"#pragma push_macro(\"__asm__\")",
		"#warning __asm__ is not available off target",
		"#undef __asm__",
	} {
		source := "#ifdef RA8_OFF_TARGET\n" + line + "\n#endif\n"
		problems := findingsIn(t, source)
		if len(problems) != 1 {
			t.Fatalf("%q: findings = %d, want 1", line, len(problems))
		}
		if problems[0].line != 2 || problems[0].text != line {
			t.Fatalf("%q: reported %d %q", line, problems[0].line, problems[0].text)
		}
	}
}

// A macro is closed out by its #endif like anything else, so asm in a
// later macro is quiet again.
func TestAMacroAfterTheConditionalClosesIsQuietAgain(t *testing.T) {
	source := strings.Join([]string{
		"#ifdef RA8_OFF_TARGET",
		"#define RA8_NOP() __asm__(\"nop\")",
		"#endif",
		"#define RA8_WFI() __asm__(\"wfi\")",
		"",
	}, "\n")

	problems := findingsIn(t, source)
	if len(problems) != 1 || problems[0].line != 2 {
		t.Fatalf("findings = %#v, want only the line inside the conditional", problems)
	}
}

// #else is deliberately NOT tracked. Everything between an off-target
// #ifdef and its #endif is judged, the else branch included, so a driver
// cannot move bare asm one branch over and go quiet. The gate's own self
// test depends on this: it expects two findings from a fixture whose asm
// sits on both sides of an #else. Pinned as behaviour.
func TestTheElseBranchOfAnOffTargetConditionalIsStillJudged(t *testing.T) {
	source := strings.Join([]string{
		"#ifdef RA8_OFF_TARGET",
		"void f(void) { __asm__(\"nop\"); }",
		"#else",
		"void g(void) { __asm__(\"wfi\"); }",
		"#endif",
		"",
	}, "\n")

	problems := findingsIn(t, source)
	if len(problems) != 2 {
		t.Fatalf("findings = %d, want both branches: %#v", len(problems), problems)
	}
	if problems[0].line != 2 || problems[1].line != 4 {
		t.Fatalf("lines = %d and %d, want 2 and 4", problems[0].line, problems[1].line)
	}

	// A macro in the else branch is the same story, and it is the case the
	// directive arm and the #else behaviour meet on.
	macro := "#ifdef RA8_OFF_TARGET\nvoid f(void) { ra8_hw_nop(); }\n#else\n#define RA8_NOP() __asm__(\"nop\")\n#endif\n"
	if got := findingsIn(t, macro); len(got) != 1 || got[0].line != 4 {
		t.Fatalf("findings = %#v, want the macro in the else branch", got)
	}
}

// A directive carrying asm nested inside an unrelated conditional still
// fires as long as an off-target conditional is open anywhere above it,
// which is what hasTrue over the whole stack buys.
func TestAnOffTargetConditionalOpenAnywhereAboveStillCounts(t *testing.T) {
	source := strings.Join([]string{
		"#ifdef RA8_OFF_TARGET",
		"#ifdef RA8_DEBUG",
		"#define RA8_NOP() __asm__(\"nop\")",
		"#endif",
		"#endif",
		"",
	}, "\n")

	problems := findingsIn(t, source)
	if len(problems) != 1 || problems[0].line != 3 {
		t.Fatalf("findings = %#v, want the nested macro line", problems)
	}

	// The other way round, the inner conditional is the off-target one.
	inner := "#ifdef RA8_DEBUG\n#ifdef RA8_OFF_TARGET\n#define RA8_NOP() __asm__(\"nop\")\n#endif\n#endif\n"
	if got := findingsIn(t, inner); len(got) != 1 || got[0].line != 3 {
		t.Fatalf("findings = %#v, want the inner off-target macro", got)
	}
}

// An #elif that brings RA8_OFF_TARGET into a conditional that did not
// start with it opens the same judgement for a macro, and the frame closes
// at the #endif.
func TestAnElifThatBringsOffTargetInJudgesTheMacrosAfterIt(t *testing.T) {
	source := strings.Join([]string{
		"#if defined(RA8_DEBUG)",
		"#define RA8_A() __asm__(\"nop\")",
		"#elif defined(RA8_OFF_TARGET)",
		"#define RA8_B() __asm__(\"wfi\")",
		"#endif",
		"#define RA8_C() __asm__(\"svc 0\")",
		"",
	}, "\n")

	problems := findingsIn(t, source)
	if len(problems) != 1 {
		t.Fatalf("findings = %d, want only the macro after the elif: %#v", len(problems), problems)
	}
	if problems[0].line != 4 {
		t.Fatalf("line = %d, want 4", problems[0].line)
	}
}

// Asm written inside a comment on a directive line is not code, and the
// comment stripper runs before any of this, so it stays quiet while the
// line count is preserved for everything after it.
func TestAsmInACommentOnADirectiveLineStaysQuiet(t *testing.T) {
	source := strings.Join([]string{
		"#ifdef RA8_OFF_TARGET",
		"#define RA8_NOP() ra8_hw_nop() /* not __asm__(\"nop\") */",
		"/* __asm__(\"wfi\")",
		"   still prose */",
		"#define RA8_WFI() __asm__(\"wfi\")",
		"#endif",
		"",
	}, "\n")

	problems := findingsIn(t, source)
	if len(problems) != 1 {
		t.Fatalf("findings = %d, want only the real macro: %#v", len(problems), problems)
	}
	if problems[0].line != 5 {
		t.Fatalf("line = %d, want 5: the block comment must not shift the count", problems[0].line)
	}
}

// A directive whose only asm sits in a trailing comment is quiet, because
// the stripper runs first. That is the pair to the case above: the token
// has to survive comment removal to count.
func TestADirectiveWhoseAsmIsOnlyInItsCommentIsQuiet(t *testing.T) {
	source := "#ifdef RA8_OFF_TARGET\n#pragma GCC diagnostic ignored \"-Wunused\" // __asm__ lives here\n#endif\n"
	if problems := findingsIn(t, source); len(problems) != 0 {
		t.Fatalf("findings = %#v, want none: the asm is inside a comment", problems)
	}
}

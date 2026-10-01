// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import (
	"context"
	"strings"
	"testing"
)

// A source can end mid-declaration: a truncated write, a file cut off by a
// full disk. The scan then finds its pattern, finds no closing parenthesis,
// and runs out of text in the same step. That has to answer "not defined"
// through the loop's own exit rather than through the no-more-matches exit,
// because the two are different lines and only one of them was ever taken.
//
// The distinction is the trailing text: an unterminated list with anything
// after it leaves the scan looking for another match, which the sibling test
// already covers. Ending exactly at the parenthesis is what closes the loop.
func TestASourceEndingAtTheParameterListIsNotADefinition(t *testing.T) {
	if defines("ra8_nsc_cut", "RA8_NSC_VENEER void ra8_nsc_cut(") {
		t.Fatal("a source truncated at the opening parenthesis was read as a definition")
	}
	// The same declaration with the list left open but text following it: the
	// other way out of the same loop, kept here so a change that collapses
	// the two exits into one cannot pass.
	if defines("ra8_nsc_cut", "RA8_NSC_VENEER void ra8_nsc_cut(void\nint later(void) { return 0; }\n") {
		t.Fatal("an unterminated parameter list was read as a definition")
	}
}

// countingContext reports cancelled only from its failAt-th Err call onward.
// The gate consults the context once per source file, once per header and once
// per declared veneer, so counting the calls is what places the cancellation
// at a chosen stage. A deadline could not: it would land wherever the box
// happened to be scheduled.
type countingContext struct {
	context.Context
	calls  *int
	failAt int
}

func (ctx countingContext) Err() error {
	*ctx.calls++
	if *ctx.calls >= ctx.failAt {
		return context.Canceled
	}
	return nil
}

// Each stage of the walk asks whether it has been cancelled, not just the
// first. A gate that checked once at the top would keep reading headers and
// keep resolving veneers after the run was abandoned, and on a large checkout
// that is the difference between stopping and finishing the whole scan.
func TestEveryStageOfTheWalkHonoursACancellation(t *testing.T) {
	for _, one := range []struct {
		name   string
		failAt int
	}{
		// One header and no .c files, so the first consultation is the
		// header loop's.
		{name: "cancelled before the headers are read", failAt: 1},
		// The header is read, then the veneer it declared is abandoned
		// before it is resolved against the sources.
		{name: "cancelled before the veneers are resolved", failAt: 2},
	} {
		t.Run(one.name, func(t *testing.T) {
			root := tree(t,
				map[string]string{"ra8_nsc.h": "RA8_NSC_VENEER void ra8_nsc_one(void);\n"},
				map[string]string{})

			calls := 0
			ctx := countingContext{Context: context.Background(), calls: &calls, failAt: one.failAt}

			var stdout, stderr strings.Builder
			if code := Run(ctx, root, nil, &stdout, &stderr); code != 2 {
				t.Fatalf("exit %d, want 2: %s%s", code, stdout.String(), stderr.String())
			}
			if !strings.Contains(stderr.String(), "cancelled") {
				t.Fatalf("the refusal does not say it was cancelled: %q", stderr.String())
			}
			// An abandoned run reports nothing about the boundary. The
			// veneer here is genuinely undefined, so a gate that carried on
			// would print a finding, and that is exactly the wrong answer to
			// hand back from a scan that never completed.
			if strings.Contains(stdout.String(), "ra8_nsc_one") {
				t.Fatalf("a cancelled scan still reported a finding: %q", stdout.String())
			}
			if calls != one.failAt {
				t.Fatalf("the context was consulted %d times, want %d", calls, one.failAt)
			}
		})
	}
}

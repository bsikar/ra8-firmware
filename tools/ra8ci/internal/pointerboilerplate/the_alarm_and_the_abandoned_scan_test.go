// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package pointerboilerplate

import (
	"context"
	"regexp"
	"strings"
	"testing"
)

// cancellingContext answers nil until the gate has asked often enough to be
// past its scope derivation, then reports the run abandoned. A context whose
// Done channel actually closed would kill the git invocation instead, and the
// refusal under test sits after that, on the scan of the derived paths.
type cancellingContext struct {
	context.Context
	calls  *int
	failAt int
}

func (ctx cancellingContext) Err() error {
	*ctx.calls++
	if *ctx.calls >= ctx.failAt {
		return context.Canceled
	}
	return nil
}

// The self-test is the only thing between a detector that stopped matching and
// a tree that reports clean forever. With the pattern blinded, the first case
// it holds has to fail by name, and the gate has to answer 1 rather than the
// 0 a passing self-test earns.
func TestASelfTestWhoseDetectorWentBlindFails(t *testing.T) {
	original := generated
	t.Cleanup(func() { generated = original })
	generated = regexp.MustCompile("zzzz-this-never-matches-zzzz")

	got := run(t, context.Background(), plantRepo(t, nil), "--selftest")
	if got.code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", got.code, got.stderr)
	}
	if !strings.Contains(got.stderr, "FAIL: plain generated form") {
		t.Errorf("stderr = %q, want the failing case named", got.stderr)
	}
	if strings.Contains(got.stdout, "PASS") {
		t.Errorf("stdout = %q, a failed self-test may not claim PASS", got.stdout)
	}
}

// A self-test that fails on a later case still names that case rather than the
// first one, which is what tells a reader which direction of the detector
// broke: the forms it must catch, or the forms it must leave alone.
func TestAFailingSelfTestNamesTheCaseThatBroke(t *testing.T) {
	original := generated
	t.Cleanup(func() { generated = original })
	// Matches every line, so the first four cases still hold and the first
	// must-not-match case is the one that breaks.
	generated = regexp.MustCompile("")

	got := run(t, context.Background(), plantRepo(t, nil), "--selftest")
	if got.code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", got.code, got.stderr)
	}
	if !strings.Contains(got.stderr, "FAIL: legacy wording") {
		t.Errorf("stderr = %q, want the over-matching case named", got.stderr)
	}
}

// The scan asks about cancellation once per file, so a run abandoned partway
// through a full scope stops there. The gate must then refuse, and must not
// print the findings it had gathered, which would read as a whole verdict over
// a scan that never finished.
func TestARunAbandonedDuringTheScanNeverReachesAVerdict(t *testing.T) {
	root := plantFullScope(t, map[string]string{
		"apps/late.c": "/* see header for the documented contract. */\n",
	})
	calls := 0
	ctx := cancellingContext{Context: context.Background(), calls: &calls, failAt: 200}

	got := run(t, ctx, root)
	if got.code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", got.code, got.stdout, got.stderr)
	}
	if !strings.Contains(got.stderr, context.Canceled.Error()) {
		t.Errorf("stderr = %q, want the cancellation carried through", got.stderr)
	}
	if strings.Contains(got.stdout, "clean") || strings.Contains(got.stdout, "apps/late.c") {
		t.Errorf("stdout = %q, an abandoned scan may not report a verdict or its partial findings", got.stdout)
	}
}

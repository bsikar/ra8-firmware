// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package waverefs

import (
	"context"
	"regexp"
	"strings"
	"testing"
)

// cancellingContext answers nil until the gate has asked often enough to be
// past its scope derivation, then reports the run abandoned. A context whose
// Done channel actually closed would stop the git invocation instead, and the
// refusal under test sits after that, on the scan.
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

// A repository whose scope is real but short of the floor is the case the
// floor exists for: the derivation worked, it just returned almost nothing,
// and reporting that as a clean tree would hide every violation in the repo.
// The scope that cannot be derived at all is a different refusal, so this one
// has to name the count and the floor.
func TestARealButShortScopeIsRefusedAgainstTheFloor(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"README.md":        "fixed in Wave 70\n",
		"infra/deploy.yml": "text\n",
	})
	var out, errs strings.Builder
	code := Run(context.Background(), root, nil, &out, &errs)
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stderr %q)", code, errs.String())
	}
	if !strings.Contains(errs.String(), "floor is 2500") {
		t.Errorf("stderr = %q, want the floor named", errs.String())
	}
	if !strings.Contains(errs.String(), "2 file(s) in scope") {
		t.Errorf("stderr = %q, want the derived count named", errs.String())
	}
	if strings.Contains(out.String(), "gate clean") || strings.Contains(out.String(), "violations") {
		t.Errorf("stdout = %q, must report neither a clean tree nor a verdict", out.String())
	}
}

// The scan asks about cancellation once per file, so a run abandoned halfway
// through a full scope stops there. The gate must then say the scan failed and
// exit 2, not print the findings it happened to collect first, which would read
// as a complete verdict over a scan that never finished.
func TestARunAbandonedDuringTheScanIsNotReportedAsAVerdict(t *testing.T) {
	root := plantFullScope(t, map[string]string{
		"docs/plan.md": "the Wave 3 rollout\n",
	})
	calls := 0
	ctx := cancellingContext{Context: context.Background(), calls: &calls, failAt: 200}
	var out, errs strings.Builder
	code := Run(ctx, root, nil, &out, &errs)
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out.String(), errs.String())
	}
	if !strings.Contains(errs.String(), "scan failed") {
		t.Errorf("stderr = %q, want the scan named as the failure", errs.String())
	}
	if !strings.Contains(errs.String(), context.Canceled.Error()) {
		t.Errorf("stderr = %q, want the cancellation carried through", errs.String())
	}
	if strings.Contains(out.String(), "violations") || strings.Contains(out.String(), "gate clean") {
		t.Errorf("stdout = %q, an abandoned scan may not reach a verdict", out.String())
	}
}

// The self-test is the only thing standing between a broken detector and a
// tree that reports clean forever. With the pattern blinded, the first case it
// holds must fail by name, and the run must answer 1 without a PASS line.
func TestASelfTestWhoseDetectorWentBlindFails(t *testing.T) {
	original := wavePattern
	t.Cleanup(func() { wavePattern = original })
	wavePattern = regexp.MustCompile("zzzz-this-never-matches-zzzz")

	root := plantRepo(t, map[string]string{
		"infra/deploy.yml": "text\n",
		"just/build.just":  "text\n",
	})
	var out, errs strings.Builder
	code := Run(context.Background(), root, []string{"--selftest"}, &out, &errs)
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs.String())
	}
	if !strings.Contains(errs.String(), "FAIL: numbered wave") {
		t.Errorf("stderr = %q, want the failing case named", errs.String())
	}
	if strings.Contains(out.String(), "PASS") {
		t.Errorf("stdout = %q, a failed self-test may not claim PASS", out.String())
	}
}

// An annotation that opens the line has nothing in front of it, and nothing is
// not a token it could be welded to. Reading the empty prefix as an attachment
// would drop the opt-out and report the line the author had already excused.
func TestAnOptOutAtTheStartOfTheLineStillExcusesIt(t *testing.T) {
	if !excused("WAVE-OK: quoting the Wave 3 milestone by name") {
		t.Error("an opt-out in the first column must excuse its line")
	}
	if excused("WAVE-OK") {
		t.Error("a bare marker in the first column states no reason and excuses nothing")
	}
}

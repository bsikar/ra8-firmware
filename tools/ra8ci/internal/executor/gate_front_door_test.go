// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"bytes"
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func spentByDeadline() context.Context {
	ctx, cancel := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	cancel()
	return ctx
}

func spentByCancel() context.Context {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	return ctx
}

func TestALiveContextLetsAGateStart(t *testing.T) {
	if _, spent := gateRefusedBeforeStart(context.Background()); spent {
		t.Fatal("a context with nothing wrong with it must not refuse the gate")
	}
}

func TestASpentDeadlineRefusesTheGateAsATimeout(t *testing.T) {
	refusal, spent := gateRefusedBeforeStart(spentByDeadline())
	if !spent || !refusal.TimedOut || refusal.Cancelled {
		t.Fatalf("an expired deadline is a timeout: %+v spent=%t", refusal, spent)
	}
	if refusal.ExitCode != noChildExit {
		t.Fatalf("nothing judged anything, so there is no exit code: %+v", refusal)
	}
}

func TestACancelledContextRefusesTheGateAsACancellation(t *testing.T) {
	refusal, spent := gateRefusedBeforeStart(spentByCancel())
	if !spent || refusal.TimedOut || !refusal.Cancelled {
		t.Fatalf("a cancellation is not a timeout: %+v spent=%t", refusal, spent)
	}
	if refusal.ExitCode != noChildExit {
		t.Fatalf("nothing judged anything, so there is no exit code: %+v", refusal)
	}
}

// The two dispatch paths answer the same question the same way. runCommand is
// the external door and gateRefusedBeforeStart is the built-in one; if a later
// edit teaches one of them a different answer this fails.
func TestBothDoorsRefuseASpentContextInTheSameWords(t *testing.T) {
	for name, spentContext := range map[string]func() context.Context{
		"deadline": spentByDeadline,
		"cancel":   spentByCancel,
	} {
		ctx := spentContext()
		external, err := runCommand(ctx, "/bin/true", nil, t.TempDir(), nil, nil, nil, time.Millisecond)
		if err != nil {
			t.Fatalf("%s: the external door reports the refusal in the result, not as an error: %v", name, err)
		}
		builtin, spent := gateRefusedBeforeStart(ctx)
		if !spent {
			t.Fatalf("%s: the built-in door let a spent context through", name)
		}
		if external != builtin {
			t.Fatalf("%s: external door says %+v and the built-in door says %+v", name, external, builtin)
		}
	}
}

// The whole point of the refusal is that the gate never runs, so a gate step
// on a spent context reports no verdict at all rather than the standard's.
func TestAGateStepOnASpentContextReportsNoVerdict(t *testing.T) {
	var stdout, stderr bytes.Buffer
	step := catalog.Step{Name: "ascii", Program: "ra8ci:ascii", Args: []string{"--this-would-be-scanned"}}
	result, err := runStep(spentByDeadline(), t.TempDir(), nil, step, &stdout, &stderr, time.Millisecond)
	if err != nil {
		t.Fatalf("a refused step is an outcome, not an error: %v", err)
	}
	if result.ExitCode != noChildExit {
		t.Fatalf("a gate that never ran cannot report %d: %+v", result.ExitCode, result)
	}
	if !result.TimedOut || result.Cancelled {
		t.Fatalf("the step carries the ending that refused it: %+v", result)
	}
	if stdout.Len() != 0 || stderr.Len() != 0 {
		t.Fatalf("a refused gate wrote output: stdout=%q stderr=%q", stdout.String(), stderr.String())
	}
}

// A refused step still accounts for its own logs, the way a refused external
// step does: the digest of nothing and no bytes, never an empty field.
func TestARefusedGateStepStillAccountsForItsLogs(t *testing.T) {
	const digestOfNothing = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	step := catalog.Step{Name: "since", Program: "ra8ci:since"}
	result, err := runStep(spentByCancel(), t.TempDir(), nil, step, &bytes.Buffer{}, &bytes.Buffer{}, time.Millisecond)
	if err != nil {
		t.Fatalf("a refused step is an outcome, not an error: %v", err)
	}
	if result.StdoutSHA256 != digestOfNothing || result.StderrSHA256 != digestOfNothing {
		t.Fatalf("a refused step must still digest its empty logs: %+v", result)
	}
	if result.StdoutBytes != 0 || result.StderrBytes != 0 {
		t.Fatalf("a refused step wrote bytes: %+v", result)
	}
	if result.Name != step.Name || result.StartedAt.IsZero() || result.EndedAt.IsZero() {
		t.Fatalf("a refused step is still a step with a name and a span: %+v", result)
	}
}

// The refusal is only a front door. A gate that was allowed to start keeps its
// answer, which is what the external path does with a child that exits under
// the wire.
func TestAGateThatWasAllowedToStartKeepsItsAnswer(t *testing.T) {
	step := catalog.Step{Name: "runner-clock", Program: "ra8ci:runner-clock", Args: []string{"--selftest"}}
	result, err := runStep(context.Background(), t.TempDir(), nil, step, &bytes.Buffer{}, &bytes.Buffer{}, time.Millisecond)
	if err != nil {
		t.Fatalf("selftest step failed: %v", err)
	}
	if result.ExitCode != 0 {
		t.Fatalf("a gate the context allowed to run reports its own verdict: %+v", result)
	}
	if result.TimedOut || result.Cancelled {
		t.Fatalf("nothing ended this step early: %+v", result)
	}
}

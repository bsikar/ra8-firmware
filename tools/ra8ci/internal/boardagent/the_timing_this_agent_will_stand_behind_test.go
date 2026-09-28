// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

type refusingHistoryClient struct {
	*testSegmentControlClient
	err error
}

func (c *refusingHistoryClient) HILObservations(context.Context, string, catalog.Task) (hilspec.Workload, []hilspec.HistoricalObservation, error) {
	return hilspec.Workload{}, nil, c.err
}

// timedHILFixture writes the pinned manifest and returns the checkout root, the
// workload the history is expected to describe, and the task that agrees with
// both. Each refusal below takes this agreement and breaks exactly one thing.
func timedHILFixture(t *testing.T) (string, hilspec.Workload, catalog.Task) {
	t.Helper()
	root := t.TempDir()
	manifest := filepath.Join(root, "examples", "test", "hil.conf")
	if err := os.MkdirAll(filepath.Dir(manifest), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(manifest,
		[]byte("HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=12\nHIL_EXPECT=\"demo: verdict=PASS\"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	workload := hilspec.Workload{ManifestPath: "examples/test/hil.conf", BoardModel: "EK-RA8D2",
		FixtureRevision: "fixture-v2", ProfileSHA256: "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
		ProgramFamily: "uart-demo", Mode: hilspec.ModeUARTScrape}
	task := catalog.Task{Name: "uart-demo", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "exclusive",
		Retry: catalog.RetryPolicy{MaxAttempts: 1},
		Steps: []catalog.Step{{Name: "observe", Program: "ra8ci:hil-observe"},
			{Name: "post-observe-checkpoint", Program: "noop"}},
		HIL: &catalog.HILTask{BoardID: "ek-ra8d2", BoardModel: workload.BoardModel,
			ManifestPath: workload.ManifestPath, ProgramFamily: workload.ProgramFamily,
			Mode: string(workload.Mode), ObservationStep: "observe", FlashRestoreSeconds: 10,
			TimeoutDeclared: true, TimeoutSeconds: 12}}
	return root, workload, task
}

// The timing decision is what a lease is sized from, so every way the ask can
// be wrong is refused as an invalid agent BEFORE the manifest is read. None of
// these may fall through to a decision the caller would then reserve hardware
// against.
func TestHILTimingDecisionRefusesAnAskItCannotStandBehind(t *testing.T) {
	root, workload, sound := timedHILFixture(t)

	noBoardPolicy := sound
	noBoardPolicy.BoardPolicy = "shared"

	otherBoard := sound
	otherHIL := *sound.HIL
	otherHIL.BoardID = "ek-ra8m1"
	otherBoard.HIL = &otherHIL

	noRestore := sound
	restoreHIL := *sound.HIL
	restoreHIL.FlashRestoreSeconds = 0
	noRestore.HIL = &restoreHIL

	notHIL := sound
	notHIL.Scope = "unit"

	noHILBlock := sound
	noHILBlock.HIL = nil

	unreviewed := sound
	unreviewed.Version = 0

	for name, ask := range map[string]struct {
		root string
		task catalog.Task
	}{
		"no checkout root":                {"", sound},
		"a shared board":                  {root, noBoardPolicy},
		"another board's task":            {root, otherBoard},
		"no flash restore":                {root, noRestore},
		"a task that is not HIL":          {root, notHIL},
		"a HIL task with no HIL block":    {root, noHILBlock},
		"a task the catalog would refuse": {root, unreviewed},
	} {
		agent, _, _ := newActiveSegmentAgent(t)
		client := &timingHistoryClient{
			testSegmentControlClient: agent.client.(*testSegmentControlClient), workload: workload}
		agent.client = client
		if _, err := agent.HILTimingDecision(context.Background(), ask.root, ask.task, 20*time.Second); !errors.Is(err, ErrInvalidAgent) {
			t.Errorf("%s: err = %v, want ErrInvalidAgent", name, err)
		}
	}

	agent, _, _ := newActiveSegmentAgent(t)
	client := &timingHistoryClient{
		testSegmentControlClient: agent.client.(*testSegmentControlClient), workload: workload}
	agent.client = client
	var noContext context.Context
	if _, err := agent.HILTimingDecision(noContext, root, sound, 20*time.Second); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no context: err = %v, want ErrInvalidAgent", err)
	}
	var absent *Agent
	if _, err := absent.HILTimingDecision(context.Background(), root, sound, 20*time.Second); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no agent: err = %v, want ErrInvalidAgent", err)
	}
}

// A control client that cannot answer for history is refused outright rather
// than silently timed from the manifest alone: the caller asked for the
// evidence-backed estimate and would otherwise be handed the fallback without
// being told the cohort was never consulted.
func TestHILTimingDecisionRefusesAClientThatHoldsNoHistory(t *testing.T) {
	root, _, task := timedHILFixture(t)
	agent, _, _ := newActiveSegmentAgent(t)
	if _, err := agent.HILTimingDecision(context.Background(), root, task, 20*time.Second); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("err = %v, want ErrInvalidAgent", err)
	}
}

// The manifest on disk is the pinned one. A manifest that cannot be read, or
// that disagrees with the reviewed catalog about the mode or the timing, stops
// the decision rather than letting the two drift apart unnoticed.
func TestHILTimingDecisionHoldsTheManifestToTheReviewedCatalog(t *testing.T) {
	sameAgent := func(t *testing.T, workload hilspec.Workload) *Agent {
		t.Helper()
		agent, _, _ := newActiveSegmentAgent(t)
		agent.client = &timingHistoryClient{
			testSegmentControlClient: agent.client.(*testSegmentControlClient), workload: workload}
		return agent
	}

	root, workload, task := timedHILFixture(t)
	if err := os.Remove(filepath.Join(root, "examples", "test", "hil.conf")); err != nil {
		t.Fatal(err)
	}
	if _, err := sameAgent(t, workload).HILTimingDecision(context.Background(), root, task, 20*time.Second); err == nil {
		t.Fatal("an absent manifest was timed")
	}

	root, workload, task = timedHILFixture(t)
	if err := os.WriteFile(filepath.Join(root, "examples", "test", "hil.conf"),
		[]byte("HIL_MODE=alive\nHIL_TIMEOUT_S=12\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := sameAgent(t, workload).HILTimingDecision(context.Background(), root, task, 20*time.Second); !errors.Is(err, hilspec.ErrInvalidManifest) {
		t.Fatalf("a manifest in another mode: err = %v, want ErrInvalidManifest", err)
	}

	root, workload, task = timedHILFixture(t)
	drifted := *task.HIL
	drifted.TimeoutSeconds = 11
	task.HIL = &drifted
	if _, err := sameAgent(t, workload).HILTimingDecision(context.Background(), root, task, 20*time.Second); !errors.Is(err, catalog.ErrInvalidCatalog) {
		t.Fatalf("catalog timing differing from the manifest: err = %v, want ErrInvalidCatalog", err)
	}
}

// The history the cohort is drawn from has to be the history for THIS task on
// THIS board. A workload describing something else, or one carrying no fixture
// identity at all, is refused rather than averaged in.
func TestHILTimingDecisionRefusesHistoryForAnotherWorkload(t *testing.T) {
	root, sound, task := timedHILFixture(t)

	otherManifest := sound
	otherManifest.ManifestPath = "examples/other/hil.conf"

	otherModel := sound
	otherModel.BoardModel = "EK-RA8M1"

	otherFamily := sound
	otherFamily.ProgramFamily = "spi-demo"

	otherMode := sound
	otherMode.Mode = hilspec.ModeRTTScrape

	noFixture := sound
	noFixture.FixtureRevision = ""

	noProfile := sound
	noProfile.ProfileSHA256 = ""

	for name, workload := range map[string]hilspec.Workload{
		"another manifest":    otherManifest,
		"another board model": otherModel,
		"another program":     otherFamily,
		"another mode":        otherMode,
		"no fixture revision": noFixture,
		"no fixture profile":  noProfile,
		"nothing at all":      {},
	} {
		agent, _, _ := newActiveSegmentAgent(t)
		agent.client = &timingHistoryClient{
			testSegmentControlClient: agent.client.(*testSegmentControlClient), workload: workload}
		if _, err := agent.HILTimingDecision(context.Background(), root, task, 20*time.Second); !errors.Is(err, hilspec.ErrInvalidHistory) {
			t.Errorf("%s: err = %v, want ErrInvalidHistory", name, err)
		}
	}
}

// A history lookup that fails is handed back as itself. Timing the task from
// the manifest fallback instead would read as an evidence-backed decision made
// over an empty cohort, which is the one thing the caller must not believe.
func TestHILTimingDecisionHandsBackAFailedHistoryLookup(t *testing.T) {
	root, _, task := timedHILFixture(t)
	agent, _, _ := newActiveSegmentAgent(t)
	unreachable := errors.New("board history is unreachable")
	agent.client = &refusingHistoryClient{
		testSegmentControlClient: agent.client.(*testSegmentControlClient), err: unreachable}
	if _, err := agent.HILTimingDecision(context.Background(), root, task, 20*time.Second); !errors.Is(err, unreachable) {
		t.Fatalf("err = %v, want the lookup failure itself", err)
	}
}

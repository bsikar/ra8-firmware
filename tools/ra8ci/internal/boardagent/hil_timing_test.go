// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type timingHistoryClient struct {
	*testSegmentControlClient
	workload hilspec.Workload
	rows     []hilspec.HistoricalObservation
}

func (c *timingHistoryClient) HILObservations(context.Context, string, catalog.Task) (hilspec.Workload, []hilspec.HistoricalObservation, error) {
	return c.workload, c.rows, nil
}

func TestHILTimingDecisionUsesManifestFallbackThenObservedCohort(t *testing.T) {
	root := t.TempDir()
	manifest := filepath.Join(root, "examples", "test", "hil.conf")
	if err := os.MkdirAll(filepath.Dir(manifest), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(manifest, []byte("HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=12\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	workload := hilspec.Workload{ManifestPath: "examples/test/hil.conf", BoardModel: "EK-RA8D2",
		FixtureRevision: "fixture-v2", ProfileSHA256: "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
		ProgramFamily: "uart-demo", Mode: hilspec.ModeUARTScrape}
	task := catalog.Task{Name: "uart-demo", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "exclusive", Retry: catalog.RetryPolicy{MaxAttempts: 1},
		Steps: []catalog.Step{{Name: "observe", Program: "ra8ci:hil-observe"}, {Name: "post-observe-checkpoint", Program: "noop"}},
		HIL: &catalog.HILTask{BoardID: "ek-ra8d2", BoardModel: workload.BoardModel,
			ManifestPath: workload.ManifestPath, ProgramFamily: workload.ProgramFamily, Mode: string(workload.Mode),
			ObservationStep: "observe", FlashRestoreSeconds: 10}}
	agent, segmentClient, token := newActiveSegmentAgent(t)
	client := &timingHistoryClient{testSegmentControlClient: agent.client.(*testSegmentControlClient),
		workload: workload}
	agent.client = client
	decision, err := agent.HILTimingDecision(context.Background(), root, task, 20*time.Second)
	if err != nil || decision.ValidityWindow != 12*time.Second || decision.Source != "hil.conf" ||
		decision.MinimumLeaseBudget() != 22*time.Second {
		t.Fatalf("manifest fallback timing = %+v err=%v", decision, err)
	}
	for _, seconds := range []time.Duration{10, 11, 12, 13, 14} {
		client.rows = append(client.rows, hilspec.HistoricalObservation{Workload: workload,
			Duration: seconds * time.Second, Succeeded: true, EvidenceComplete: true})
	}
	decision, err = agent.HILTimingDecision(context.Background(), root, task, 20*time.Second)
	if err != nil || decision.ValidityWindow != 16*time.Second || decision.Source != "observed" ||
		decision.Samples != 5 || decision.MinimumLeaseBudget() != 26*time.Second {
		t.Fatalf("observation-backed timing = %+v err=%v", decision, err)
	}
	client.rows[0].Workload.ProfileSHA256 = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
	decision, err = agent.HILTimingDecision(context.Background(), root, task, 20*time.Second)
	if err != nil || decision.Samples != 4 || decision.RejectedRows != 1 {
		t.Fatalf("different fixture profile influenced timing = %+v err=%v", decision, err)
	}
	attemptStarted := time.Now().UTC()
	assignment := store.BoardHILAssignment{Attempt: store.Attempt{ID: "01996f90-3415-7cfe-8ff1-600058131aff",
		StartedAt: attemptStarted, DeadlineAt: attemptStarted.Add(time.Minute),
		TaskID: "01996f90-3415-7cfe-8ff1-600058131b11", AttemptNo: 1, State: "running"},
		Task: task, CatalogSHA256: "reviewed-catalog"}
	stepsRun := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, root, assignment, 20*time.Second, 0,
		func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			stepsRun++
			waiter := board.Waiter{ID: "01996f90-3415-7cfe-8ff1-600058131b01",
				LeaseID: "01996f90-3415-7cfe-8ff1-600058131b02", Holder: "human",
				Class: board.ClassHuman, Reason: "human requested fixture", Duration: time.Minute}
			next, _, enqueueErr := board.Apply(segmentClient.state, board.Enqueue{Actor: "human", Waiter: waiter}, time.Now().UTC())
			segmentClient.state = next
			return 0, enqueueErr
		})
	if err != nil || completion.Result != "preempted" || len(completion.Steps) != 1 ||
		completion.Steps[0].Key != task.HIL.ObservationStep || stepsRun != 1 ||
		segmentClient.completed != 1 || len(segmentClient.finishes) != 1 ||
		segmentClient.finishes[0] != "completed" || segmentClient.lastSegment.RecoveryMarginMS != 10000 {
		t.Fatalf("HIL task did not stop and persist at cooperative checkpoint: completion=%+v stepsRun=%d finished=%v err=%v",
			completion, stepsRun, segmentClient.finishes, err)
	}
}

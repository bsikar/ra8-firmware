// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

// hilRootHolding is hilAttemptRoot with the manifest body under test, so a
// checkout that DISAGREES with the timing the server pinned can be built.
func hilRootHolding(t *testing.T, body string) string {
	t.Helper()
	root := t.TempDir()
	manifest := filepath.Join(root, "examples", "test", "hil.conf")
	if err := os.MkdirAll(filepath.Dir(manifest), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(manifest, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return root
}

// A checkout with no manifest at all is not a failed board: the attempt is
// refused where the evidence should have been read, and the board is never
// touched.
func TestAnAttemptOverACheckoutWithNoManifestIsRefused(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	steps := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, t.TempDir(), assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			steps++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if steps != 0 || client.begins != 0 {
		t.Fatalf("an unreadable manifest reached the board: steps=%d begins=%d", steps, client.begins)
	}
	if completion.Result != "failed" || completion.Reason == "" || completion.Reason == "HIL attempt failed" {
		t.Fatalf("the refusal does not carry the read that failed: %+v", completion)
	}
}

// The manifest in the checkout and the timing the server pinned are two
// independent records of the same thing. When they disagree, the attempt is
// refused rather than run on either one.
func TestAnAttemptWhoseManifestDisagreesWithThePinnedTimingIsRefused(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	root := hilRootHolding(t, "HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=13\nHIL_EXPECT=\"demo: verdict=PASS\"\n")
	steps := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, root, assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			steps++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if steps != 0 || client.begins != 0 {
		t.Fatalf("a disagreeing manifest reached the board: steps=%d begins=%d", steps, client.begins)
	}
	if !strings.Contains(completion.Reason, "differs from manifest") {
		t.Fatalf("the reason does not name the disagreement: %q", completion.Reason)
	}
}

// The workload the timing evidence was measured over has to be the workload
// this task describes, or the pinned decision is about some other program.
func TestAnAttemptWhoseTimingWasMeasuredOverAnotherWorkloadIsRefused(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	assignment.HILTiming.Workload.ProgramFamily = "some-other-demo"
	steps := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			steps++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if steps != 0 || client.begins != 0 {
		t.Fatalf("timing from another workload reached the board: steps=%d begins=%d", steps, client.begins)
	}
	if !strings.Contains(completion.Reason, hilspec.ErrInvalidHistory.Error()) {
		t.Fatalf("the reason does not name the history: %q", completion.Reason)
	}
}

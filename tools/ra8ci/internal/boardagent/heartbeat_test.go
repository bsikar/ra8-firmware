// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// beatingClient is a control client that can also report a holder alive. The
// server's answer is whatever the test sets, so the loop's policy is what is
// under test and never the transport.
type beatingClient struct {
	*testSegmentControlClient

	mu       sync.Mutex
	beats    int
	at       []time.Time
	interval time.Duration
	answer   func(int) (board.Snapshot, boardclient.HolderLiveness, error)
}

func (c *beatingClient) Heartbeat(_ context.Context, token boardclient.LeaseToken) (board.Snapshot, boardclient.HolderLiveness, error) {
	c.mu.Lock()
	c.beats++
	beat := c.beats
	c.at = append(c.at, time.Now())
	answer, interval := c.answer, c.interval
	state := c.state
	c.mu.Unlock()
	if answer != nil {
		return answer(beat)
	}
	return state, boardclient.HolderLiveness{Held: true, LeaseID: token.LeaseID,
		Holder: "agent", Beat: true, LastSeenAt: time.Now().UTC(), Interval: interval}, nil
}

func (c *beatingClient) count() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.beats
}

func newBeatingAgent(t *testing.T) (*Agent, *beatingClient, boardclient.LeaseToken) {
	t.Helper()
	agent, segment, token := newActiveSegmentAgent(t)
	client := &beatingClient{testSegmentControlClient: segment, interval: minHeartbeatInterval}
	agent.client = client
	return agent, client, token
}

func TestBeatIntervalStaysInsideTheRangeLivenessIsJudgedOver(t *testing.T) {
	cases := []struct {
		reported time.Duration
		want     time.Duration
	}{
		{0, defaultHeartbeatInterval},
		{-time.Second, defaultHeartbeatInterval},
		{time.Nanosecond, minHeartbeatInterval},
		{minHeartbeatInterval, minHeartbeatInterval},
		{30 * time.Second, 30 * time.Second},
		{board.MaxHeartbeatInterval, board.MaxHeartbeatInterval},
		{board.MaxHeartbeatInterval + time.Minute, board.MaxHeartbeatInterval},
	}
	for _, c := range cases {
		if got := beatInterval(c.reported); got != c.want {
			t.Fatalf("beat interval for %s = %s, want %s", c.reported, got, c.want)
		}
	}
}

func TestReportAliveRefusesBeforeReachingTheServer(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	other := token
	other.BoardID = "ek-ra8m1"
	stale := token
	stale.Generation = 0
	empty := token
	empty.LeaseID = "not-a-uuid"
	for name, bad := range map[string]boardclient.LeaseToken{
		"another board": other, "no generation": stale, "no lease": empty,
	} {
		if _, err := agent.ReportAlive(context.Background(), bad); !errors.Is(err, ErrInvalidAgent) {
			t.Fatalf("%s token was accepted: %v", name, err)
		}
	}
	if client.count() != 0 {
		t.Fatalf("a refused token still reached the server: beats=%d", client.count())
	}
}

func TestReportAliveNeedsAClientThatCanBeat(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	if _, err := agent.ReportAlive(context.Background(), token); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("a client with no heartbeat reported a holder alive: %v", err)
	}
	if err := agent.KeepAlive(context.Background(), token); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("a client with no heartbeat started a reporting loop: %v", err)
	}
}

func TestReportAliveRefusesAnAnswerAboutAnotherLease(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		return client.state, boardclient.HolderLiveness{Held: true,
			LeaseID: "01996f90-3415-7cfe-8ff1-600058131b02", Holder: "someone else"}, nil
	}
	if _, err := agent.ReportAlive(context.Background(), token); !errors.Is(err, boardclient.ErrInvalidRequest) {
		t.Fatalf("liveness for another lease was taken as this holder's: %v", err)
	}
}

func TestReportAliveRefusesABoardThatNoLongerRecordsThisLease(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	regenerated := client.state
	lease := *regenerated.Lease
	lease.Generation++
	regenerated.Lease = &lease
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		return regenerated, boardclient.HolderLiveness{Held: true, LeaseID: token.LeaseID}, nil
	}
	if _, err := agent.ReportAlive(context.Background(), token); !errors.Is(err, boardclient.ErrStaleLease) {
		t.Fatalf("a regenerated lease was reported alive: %v", err)
	}
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		released := client.state
		released.Lease = nil
		return released, boardclient.HolderLiveness{}, nil
	}
	if _, err := agent.ReportAlive(context.Background(), token); !errors.Is(err, boardclient.ErrStaleLease) {
		t.Fatalf("a released board was reported alive: %v", err)
	}
}

func TestKeepAliveReportsOnTheIntervalTheServerHandsBack(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client.answer = func(beat int) (board.Snapshot, boardclient.HolderLiveness, error) {
		if beat == 3 {
			cancel()
		}
		return client.state, boardclient.HolderLiveness{Held: true, LeaseID: token.LeaseID,
			Beat: true, Interval: minHeartbeatInterval}, nil
	}
	started := time.Now()
	if err := agent.KeepAlive(ctx, token); err != nil {
		t.Fatalf("a cancelled holder loop reported an error: %v", err)
	}
	if client.count() != 3 {
		t.Fatalf("holder beat %d times, want 3", client.count())
	}
	// Three beats at the interval the server asked for cannot have run in
	// less than the two waits between them.
	if elapsed := time.Since(started); elapsed < 2*minHeartbeatInterval {
		t.Fatalf("three beats took %s, faster than the interval the server asked for", elapsed)
	}
}

func TestKeepAliveStopsWhenTheLeaseStoppedBeingOurs(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		return board.Snapshot{}, boardclient.HolderLiveness{}, boardclient.ErrStaleLease
	}
	err := agent.KeepAlive(context.Background(), token)
	if !errors.Is(err, boardclient.ErrStaleLease) {
		t.Fatalf("a superseded holder kept reporting: %v", err)
	}
	if client.count() != 1 {
		t.Fatalf("a superseded holder beat %d times, want 1", client.count())
	}
}

func TestKeepAliveOutlivesATransportBlip(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	blip := errors.New("connection reset by peer")
	client.answer = func(beat int) (board.Snapshot, boardclient.HolderLiveness, error) {
		switch beat {
		case 1, 2:
			return board.Snapshot{}, boardclient.HolderLiveness{}, blip
		case 3:
			cancel()
		}
		return client.state, boardclient.HolderLiveness{Held: true, LeaseID: token.LeaseID,
			Beat: true, Interval: minHeartbeatInterval}, nil
	}
	if err := agent.KeepAlive(ctx, token); err != nil {
		t.Fatalf("a blip ended the reporting loop: %v", err)
	}
	if client.count() != 3 {
		t.Fatalf("holder beat %d times through a blip, want 3", client.count())
	}
}

func TestKeepAliveLeavesTheDeadlineAlone(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	before := *client.state.Lease
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client.answer = func(beat int) (board.Snapshot, boardclient.HolderLiveness, error) {
		if beat == 2 {
			cancel()
		}
		return client.state, boardclient.HolderLiveness{Held: true, LeaseID: token.LeaseID,
			Beat: true, Interval: minHeartbeatInterval,
			// An overdue holder is a report, never a verdict.
			Overdue: true, ExpiresAt: before.ExpiresAt}, nil
	}
	if err := agent.KeepAlive(ctx, token); err != nil {
		t.Fatalf("an overdue holder was stopped by its own report: %v", err)
	}
	after := *client.state.Lease
	if !after.ExpiresAt.Equal(before.ExpiresAt) || after.Generation != before.Generation {
		t.Fatalf("reporting alive moved the lease: before=%+v after=%+v", before, after)
	}
}

// TestRunHILAttemptReportsTheHolderAliveWhileItWorks is the caller half: a
// long step is exactly the window in which a crashed holder is invisible, so
// the attempt has to be beating through it.
func TestRunHILAttemptReportsTheHolderAliveWhileItWorks(t *testing.T) {
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
		Steps: []catalog.Step{{Name: "observe", Program: "ra8ci:hil-observe"}},
		HIL: &catalog.HILTask{BoardID: "ek-ra8d2", BoardModel: workload.BoardModel,
			ManifestPath: workload.ManifestPath, ProgramFamily: workload.ProgramFamily, Mode: string(workload.Mode),
			ObservationStep: "observe", FlashRestoreSeconds: 10, TimeoutDeclared: true, TimeoutSeconds: 12}}
	agent, client, token := newBeatingAgent(t)
	started := time.Now().UTC()
	assignment := store.BoardHILAssignment{Attempt: store.Attempt{ID: "01996f90-3415-7cfe-8ff1-600058131aff",
		StartedAt: started, DeadlineAt: started.Add(time.Minute),
		TaskID: "01996f90-3415-7cfe-8ff1-600058131b11", AttemptNo: 1, State: "running"},
		Task: task, CatalogSHA256: "reviewed-catalog",
		HILTiming: &store.HILTimingEvidence{Workload: workload,
			Decision: hilspec.Decision{ValidityWindow: 12 * time.Second, Source: "hil.conf",
				FlashRestoreBound: 10 * time.Second}}}
	completion, err := agent.RunHILAttempt(context.Background(), token, root, assignment, 20*time.Second, 0,
		func(ctx context.Context, _ string, _ catalog.Task, _ catalog.Step) (int, error) {
			// Long enough for the loop to report more than its first beat.
			select {
			case <-ctx.Done():
			case <-time.After(3 * minHeartbeatInterval):
			}
			return 0, nil
		})
	if err != nil || completion.Result != "succeeded" {
		t.Fatalf("HIL attempt did not succeed: completion=%+v err=%v", completion, err)
	}
	beats := client.count()
	if beats < 2 {
		t.Fatalf("holder beat %d times across a step it spent three intervals in", beats)
	}
	// Nothing keeps beating at a board this attempt has already left.
	time.Sleep(2 * minHeartbeatInterval)
	if after := client.count(); after != beats {
		t.Fatalf("holder kept beating after the attempt returned: %d then %d", beats, after)
	}
}

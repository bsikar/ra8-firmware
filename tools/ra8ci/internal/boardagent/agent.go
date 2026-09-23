// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package boardagent reconciles a board's durable generation with PostgreSQL
// lease state and runs bounded callbacks at serialized hardware checkpoints.
// Concrete device adapters remain separate.
package boardagent

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

var ErrInvalidAgent = errors.New("invalid persistent board agent configuration")

const maxBoardOperation = time.Hour

type ControlClient interface {
	Status(context.Context, string) (board.Snapshot, error)
	AcknowledgeGrant(context.Context, boardclient.LeaseToken) (board.Snapshot, error)
	ObserveAgentGeneration(context.Context, string, uint64) (board.Snapshot, error)
}

// SegmentClient is the authenticated server API used to atomically begin and
// finish durable board operations under the current lease generation.
type SegmentClient interface {
	BeginSegment(context.Context, boardclient.LeaseToken, string, string, time.Duration, time.Duration) (store.BoardSegment, error)
	FinishSegment(context.Context, boardclient.LeaseToken, string, string, string) error
}

// Agent reconciles one persistent physical board agent identity.
type Agent struct {
	boardID     string
	client      ControlClient
	highWater   HighWaterStore
	interval    time.Duration
	clock       func() time.Time
	fenceMu     sync.Mutex
	fence       *board.DeadlineFence
	segmentGate chan struct{}
}

// New constructs a single-board reconciler. Its service identity must be
// authorized by the server for board-agent actions.
func New(boardID string, client ControlClient, highWater HighWaterStore, interval time.Duration) (*Agent, error) {
	if !validBoardID(boardID) || client == nil || highWater == nil ||
		interval < 250*time.Millisecond || interval > 30*time.Second {
		return nil, ErrInvalidAgent
	}
	return &Agent{boardID: boardID, client: client, highWater: highWater,
		interval: interval, clock: time.Now, segmentGate: make(chan struct{}, 1)}, nil
}

// Reconcile observes the server lease, persists any new grant generation
// before acknowledging it, and quarantines a database restore behind local
// state. It never grants itself hardware authority.
func (a *Agent) Reconcile(ctx context.Context) (board.Snapshot, error) {
	if a == nil || ctx == nil || a.segmentGate == nil {
		return board.Snapshot{}, ErrInvalidAgent
	}
	if err := a.enterSegment(ctx); err != nil {
		return board.Snapshot{}, err
	}
	defer a.leaveSegment()
	snapshot, err := a.client.Status(ctx, a.boardID)
	if err != nil {
		return board.Snapshot{}, err
	}
	localHighWater, err := a.highWater.Load()
	if err != nil {
		return board.Snapshot{}, fmt.Errorf("read durable board generation: %w", err)
	}
	if localHighWater > snapshot.Generation || localHighWater < snapshot.AgentHighWater {
		return a.client.ObserveAgentGeneration(ctx, a.boardID, localHighWater)
	}
	if snapshot.Phase == board.GrantPending {
		if snapshot.Lease == nil || snapshot.Lease.Generation != snapshot.Generation {
			return board.Snapshot{}, boardclient.ErrStaleLease
		}
		if snapshot.Generation > localHighWater {
			if err := a.highWater.Advance(snapshot.Generation); err != nil {
				return board.Snapshot{}, fmt.Errorf("persist board generation before grant acknowledgement: %w", err)
			}
			localHighWater = snapshot.Generation
		}
		if localHighWater != snapshot.Generation {
			return a.client.ObserveAgentGeneration(ctx, a.boardID, localHighWater)
		}
		token := boardclient.LeaseToken{BoardID: a.boardID, RequestID: snapshot.Lease.WaiterID,
			LeaseID: snapshot.Lease.ID, Generation: snapshot.Lease.Generation,
			ExpiresAt: snapshot.Lease.ExpiresAt, Version: snapshot.Version}
		acknowledged, err := a.client.AcknowledgeGrant(ctx, token)
		if err != nil {
			return acknowledged, err
		}
		if err := a.refreshDeadlineFence(acknowledged); err != nil {
			return acknowledged, err
		}
		return acknowledged, nil
	}
	if snapshot.Lease != nil && (snapshot.Phase == board.Active ||
		snapshot.Phase == board.YieldRequested || snapshot.Phase == board.Draining) {
		if snapshot.Lease.Generation != localHighWater || snapshot.AgentHighWater != localHighWater {
			return a.client.ObserveAgentGeneration(ctx, a.boardID, localHighWater)
		}
		if snapshot.Phase == board.Active {
			if err := a.refreshDeadlineFence(snapshot); err != nil {
				return snapshot, err
			}
		}
	} else {
		a.clearDeadlineFence()
	}
	return snapshot, nil
}

// CanStartSegment requires independent authorization from the server snapshot,
// the locally persisted generation, and a local monotonic lease deadline. The
// bound is the indivisible operation duration; recoveryMargin reserves time to
// restore a known-safe fixture before the lease expires.
func (a *Agent) CanStartSegment(ctx context.Context, token boardclient.LeaseToken,
	bound, recoveryMargin time.Duration) error {
	if a == nil || ctx == nil || token.BoardID != a.boardID || token.LeaseID == "" || token.Generation == 0 {
		return &board.Error{Code: board.InvalidArgument, Detail: "invalid board-agent segment token"}
	}
	snapshot, err := a.client.Status(ctx, a.boardID)
	if err != nil {
		return err
	}
	localHighWater, err := a.highWater.Load()
	if err != nil {
		return fmt.Errorf("read durable board generation: %w", err)
	}
	if localHighWater != token.Generation || snapshot.AgentHighWater != token.Generation {
		return &board.Error{Code: board.RecoveryNecessary, Detail: "server and durable board generations do not authorize this segment"}
	}
	now := a.clock()
	serverToken := board.Token{BoardID: token.BoardID, LeaseID: token.LeaseID, Generation: token.Generation}
	if err := board.CanStartSegment(snapshot, serverToken, now.UTC(), bound, recoveryMargin); err != nil {
		return err
	}
	if err := a.refreshDeadlineFence(snapshot); err != nil {
		return err
	}
	a.fenceMu.Lock()
	fence := a.fence
	a.fenceMu.Unlock()
	if fence == nil {
		return &board.Error{Code: board.RecoveryNecessary, Detail: "local board deadline fence is absent"}
	}
	return fence.CanStartSegment(token.Generation, now, bound, recoveryMargin)
}

// RunSegment executes one indivisible, context-bounded board operation. The
// local gate serializes reconciliation and hardware work in this process; the
// server transaction orders it against human waiters and other lease changes.
// The operation must honor ctx and must not return while child processes remain.
func (a *Agent) RunSegment(ctx context.Context, token boardclient.LeaseToken, attemptID, key string,
	bound, recoveryMargin time.Duration, operation func(context.Context) error) (store.BoardSegment, error) {
	if a == nil || ctx == nil || a.segmentGate == nil || operation == nil || token.BoardID != a.boardID ||
		!store.ValidID(attemptID) || bound <= 0 || bound > maxBoardOperation || recoveryMargin < 0 || recoveryMargin > maxBoardOperation {
		return store.BoardSegment{}, ErrInvalidAgent
	}
	client, ok := a.client.(SegmentClient)
	if !ok {
		return store.BoardSegment{}, fmt.Errorf("%w: durable segment client is unavailable", ErrInvalidAgent)
	}
	if err := a.enterSegment(ctx); err != nil {
		return store.BoardSegment{}, err
	}
	defer a.leaveSegment()
	if err := a.CanStartSegment(ctx, token, bound, recoveryMargin); err != nil {
		return store.BoardSegment{}, err
	}
	requestStarted := a.clock()
	segment, err := client.BeginSegment(ctx, token, attemptID, key, bound, recoveryMargin)
	if err != nil {
		return store.BoardSegment{}, err
	}
	finish := func(outcome string) error {
		finishCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		return client.FinishSegment(finishCtx, token, attemptID, segment.ID, outcome)
	}
	remaining := bound - a.clock().Sub(requestStarted)
	if remaining <= 0 {
		return segment, errors.Join(context.DeadlineExceeded, finish("failed"))
	}
	localHighWater, err := a.highWater.Load()
	if err != nil || localHighWater != token.Generation {
		if err == nil {
			err = boardclient.ErrStaleLease
		}
		return segment, errors.Join(err, finish("failed"))
	}
	a.fenceMu.Lock()
	fence := a.fence
	a.fenceMu.Unlock()
	if fence == nil {
		return segment, errors.Join(&board.Error{Code: board.RecoveryNecessary, Detail: "local deadline fence is absent"}, finish("failed"))
	}
	if err := fence.CanStartSegment(token.Generation, a.clock(), remaining, recoveryMargin); err != nil {
		return segment, errors.Join(err, finish("failed"))
	}
	operationCtx, cancel := context.WithTimeout(ctx, remaining)
	operationErr := operation(operationCtx)
	contextErr := operationCtx.Err()
	cancel()
	outcome := "completed"
	if operationErr != nil || contextErr != nil {
		outcome = "failed"
		if errors.Is(contextErr, context.Canceled) && ctx.Err() != nil {
			outcome = "yielded"
		}
		if operationErr == nil {
			operationErr = contextErr
		}
	}
	return segment, errors.Join(operationErr, finish(outcome))
}

func (a *Agent) enterSegment(ctx context.Context) error {
	if a == nil || a.segmentGate == nil || ctx == nil {
		return ErrInvalidAgent
	}
	select {
	case a.segmentGate <- struct{}{}:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (a *Agent) leaveSegment() {
	<-a.segmentGate
}

func (a *Agent) refreshDeadlineFence(snapshot board.Snapshot) error {
	if a == nil || snapshot.Lease == nil || snapshot.Phase != board.Active ||
		snapshot.AgentHighWater != snapshot.Lease.Generation {
		return &board.Error{Code: board.RecoveryNecessary, Detail: "active lease and installed generation are required for a deadline fence"}
	}
	localNow := a.clock()
	a.fenceMu.Lock()
	defer a.fenceMu.Unlock()
	if a.fence == nil || a.fence.Generation < snapshot.Lease.Generation {
		seeded, err := board.SeedDeadline(snapshot.Lease.Generation,
			snapshot.Lease.DeadlineVersion, snapshot.Lease.ExpiresAt, localNow, board.MaxClockOffset)
		if err != nil {
			return err
		}
		a.fence = &seeded
		return nil
	}
	if a.fence.Generation != snapshot.Lease.Generation ||
		snapshot.Lease.DeadlineVersion < a.fence.Version {
		return &board.Error{Code: board.StaleGeneration, Detail: "server lease regressed behind local deadline fence"}
	}
	if snapshot.Lease.DeadlineVersion == a.fence.Version {
		return nil
	}
	refreshed, err := board.RefreshDeadline(*a.fence, snapshot.Lease.Generation,
		snapshot.Lease.DeadlineVersion, snapshot.Lease.ExpiresAt, localNow,
		board.MaxClockOffset, true)
	if err != nil {
		return err
	}
	a.fence = &refreshed
	return nil
}

func (a *Agent) clearDeadlineFence() {
	a.fenceMu.Lock()
	a.fence = nil
	a.fenceMu.Unlock()
}

// Run performs an immediate reconciliation and then maintains the fencing
// state until cancelled. Any state or transport error exits for service-manager
// restart; no new generation is assumed while disconnected.
func (a *Agent) Run(ctx context.Context) error {
	if a == nil || ctx == nil || a.segmentGate == nil {
		return ErrInvalidAgent
	}
	ticker := time.NewTicker(a.interval)
	defer ticker.Stop()
	for {
		if _, err := a.Reconcile(ctx); err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		select {
		case <-ctx.Done():
			return nil
		case <-ticker.C:
		}
	}
}

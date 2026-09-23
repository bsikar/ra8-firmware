// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package boardagent reconciles a board's durable generation with the
// PostgreSQL lease state. It does not execute board commands.
package boardagent

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

var ErrInvalidAgent = errors.New("invalid persistent board agent configuration")

type ControlClient interface {
	Status(context.Context, string) (board.Snapshot, error)
	AcknowledgeGrant(context.Context, boardclient.LeaseToken) (board.Snapshot, error)
	ObserveAgentGeneration(context.Context, string, uint64) (board.Snapshot, error)
}

// Agent reconciles one persistent physical board agent identity.
type Agent struct {
	boardID   string
	client    ControlClient
	highWater HighWaterStore
	interval  time.Duration
	clock     func() time.Time
	fenceMu   sync.Mutex
	fence     *board.DeadlineFence
}

// New constructs a single-board reconciler. Its service identity must be
// authorized by the server for board-agent actions.
func New(boardID string, client ControlClient, highWater HighWaterStore, interval time.Duration) (*Agent, error) {
	if !validBoardID(boardID) || client == nil || highWater == nil ||
		interval < 250*time.Millisecond || interval > 30*time.Second {
		return nil, ErrInvalidAgent
	}
	return &Agent{boardID: boardID, client: client, highWater: highWater,
		interval: interval, clock: time.Now}, nil
}

// Reconcile observes the server lease, persists any new grant generation
// before acknowledging it, and quarantines a database restore behind local
// state. It never grants itself hardware authority.
func (a *Agent) Reconcile(ctx context.Context) (board.Snapshot, error) {
	if a == nil || ctx == nil {
		return board.Snapshot{}, ErrInvalidAgent
	}
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
	if a == nil || ctx == nil {
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

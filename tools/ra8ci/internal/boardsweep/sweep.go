// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package boardsweep reclaims boards whose holder never came back.
//
// board.Apply expires a lease as its first act, before the command it was
// handed, so any board that is asked anything after its deadline recovers on
// the spot. Nothing asks an idle board anything. store.TickBoard is the
// server-clock transition that exists for this and has had no production
// caller: until now a holder that died at minute two of an hour lease left
// the board held, its queue unserved and its recovery unrequested until some
// unrelated request happened to arrive.
//
// The pass decides nothing about liveness. Silence is not what ends a lease
// here (a heartbeat gone quiet is reported and never enforced, by design);
// the recorded deadline is, and the reducer re-derives that judgement under
// its own clock inside the transaction that acts on it. All this does is
// give an idle board a clock.
package boardsweep

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// defaultBatch is one pass's ceiling when the caller names none.
const defaultBatch = 100

// Boards is the ledger a pass reads and ticks. It is the narrow half of
// *store.Store this package needs, so the pass is testable without a
// database and cannot reach anything else on the way past.
type Boards interface {
	ExpiredBoardLeases(ctx context.Context, now time.Time, limit int) ([]store.ExpiredBoardLease, error)
	TickBoard(ctx context.Context, boardID string, expectedVersion uint64, now time.Time) (board.Snapshot, []board.Event, error)
}

// Report is what one pass actually did.
//
// Overtaken is not a failure and is counted apart from it: it means the board
// moved between the read and the tick, and whatever moved it ran the same
// expiry first, so the lease is reclaimed either way.
type Report struct {
	Found     int
	Reclaimed int
	Overtaken int
	Failed    int
}

// Sweeper reclaims expired board leases one pass at a time. It owns no timer:
// the caller already has one, and a sweep that ran on its own schedule could
// disagree with the maintenance loop about whether the server is still
// serving.
type Sweeper struct {
	boards Boards
	batch  int
}

// New builds a sweeper over a ledger. A batch of zero takes the default page.
func New(boards Boards, batch int) (*Sweeper, error) {
	if boards == nil {
		return nil, fmt.Errorf("%w: board sweep needs a ledger", store.ErrInvalid)
	}
	if batch < 0 {
		return nil, fmt.Errorf("%w: board sweep batch %d", store.ErrInvalid, batch)
	}
	if batch == 0 {
		batch = defaultBatch
	}
	return &Sweeper{boards: boards, batch: batch}, nil
}

// Pass reclaims every expired lease it can see, and reports what it did.
//
// The report comes back whether or not the pass was complete, because the
// counts are what actually happened either way. One board's failure never
// ends the pass: the boards behind it in the page are exactly the ones this
// exists to reclaim, and a sweep that stops at the first bad row leaves them
// held for as long as that row stays bad.
func (s *Sweeper) Pass(ctx context.Context, now time.Time) (Report, error) {
	if s == nil || s.boards == nil {
		return Report{}, fmt.Errorf("%w: board sweep is not wired", store.ErrInvalid)
	}
	if ctx == nil || now.IsZero() {
		return Report{}, fmt.Errorf("%w: board sweep needs a context and a clock", store.ErrInvalid)
	}
	expired, err := s.boards.ExpiredBoardLeases(ctx, now, s.batch)
	if err != nil {
		return Report{}, fmt.Errorf("read expired board leases: %w", err)
	}
	report := Report{Found: len(expired)}
	var failures []error
	for _, lease := range expired {
		if err := ctx.Err(); err != nil {
			return report, err
		}
		reclaimed, err := s.reclaim(ctx, lease, now)
		switch {
		case err == nil && reclaimed:
			report.Reclaimed++
		case err == nil:
			report.Overtaken++
		case errors.Is(err, store.ErrConflict):
			// The version moved under the read. Whatever moved it
			// expired the lease on the way past, since every
			// transition does, so this board needs nothing from us.
			report.Overtaken++
		default:
			report.Failed++
			failures = append(failures, fmt.Errorf("reclaim board %s: %w", lease.BoardID, err))
		}
	}
	return report, errors.Join(failures...)
}

// reclaim ticks one board and says whether this pass is what expired it.
//
// The answer is read off the events rather than off the returned phase: a
// board can be in recovery for reasons that have nothing to do with a
// deadline, and the count an operator reads should mean "this pass reclaimed
// it", not "it was not held afterwards".
func (s *Sweeper) reclaim(ctx context.Context, lease store.ExpiredBoardLease, now time.Time) (bool, error) {
	_, events, err := s.boards.TickBoard(ctx, lease.BoardID, lease.Version, now)
	if err != nil {
		return false, err
	}
	for _, e := range events {
		if e.Kind == board.LeaseExpired {
			return true, nil
		}
	}
	return false, nil
}

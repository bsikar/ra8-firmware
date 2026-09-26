// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// HeartbeatClient is the holder-authenticated report that this agent is still
// alive under the lease it already holds. It is optional on a control client
// for the same reason the segment and attempt clients are: a deployment whose
// client cannot beat carries on working and is simply never seen, which is
// the state the server already describes.
type HeartbeatClient interface {
	Heartbeat(context.Context, boardclient.LeaseToken) (board.Snapshot, boardclient.HolderLiveness, error)
}

// minHeartbeatInterval floors whatever interval the server reports. It matches
// the agent's own minimum reconcile interval: a holder that may not observe
// the board faster than this may not report at itself faster either, and a
// nonsense interval near zero would otherwise spin against the server.
const minHeartbeatInterval = 250 * time.Millisecond

// defaultHeartbeatInterval is what a holder reports on before the server has
// told it anything, which is only ever the first beat of a loop.
const defaultHeartbeatInterval = time.Minute

// beatInterval bounds the reporting interval a server hands back. The server
// decides the cadence, this only refuses to be driven outside the range the
// board package already declares liveness is judged over.
func beatInterval(reported time.Duration) time.Duration {
	switch {
	case reported <= 0:
		return defaultHeartbeatInterval
	case reported < minHeartbeatInterval:
		return minHeartbeatInterval
	case reported > board.MaxHeartbeatInterval:
		return board.MaxHeartbeatInterval
	}
	return reported
}

// settledBeat reports whether a refused beat is one that reporting again
// cannot fix: the lease is no longer this caller's, or this caller is asking
// the wrong question. Anything else is a transport blip, and a blip must not
// end a holder's reporting, because being unseen is not being finished.
func settledBeat(err error) bool {
	return errors.Is(err, boardclient.ErrStaleLease) ||
		errors.Is(err, boardclient.ErrInvalidRequest) ||
		errors.Is(err, boardclient.ErrInvalidConfig) ||
		errors.Is(err, ErrInvalidAgent)
}

func validHeartbeatToken(a *Agent, token boardclient.LeaseToken) bool {
	return a != nil && token.BoardID == a.boardID &&
		store.ValidID(token.LeaseID) && token.Generation != 0
}

// ReportAlive beats once under the lease this agent holds and returns what the
// server now says about that holder.
//
// It is not a deadline command in either direction: a beat cannot lengthen the
// lease, which is what Extend is for, and a refused beat never shortens one,
// because a crashed holder is answered by waiting for expiry and then a
// reviewed recovery sequence. The liveness it returns is a report, so an
// overdue holder is still the holder.
func (a *Agent) ReportAlive(ctx context.Context, token boardclient.LeaseToken) (boardclient.HolderLiveness, error) {
	if a == nil || ctx == nil || !validHeartbeatToken(a, token) {
		return boardclient.HolderLiveness{}, ErrInvalidAgent
	}
	client, ok := a.client.(HeartbeatClient)
	if !ok {
		return boardclient.HolderLiveness{}, ErrInvalidAgent
	}
	snapshot, liveness, err := client.Heartbeat(ctx, token)
	if err != nil {
		return boardclient.HolderLiveness{}, err
	}
	if snapshot.BoardID != token.BoardID {
		return boardclient.HolderLiveness{}, boardclient.ErrInvalidRequest
	}
	// A report about another lease is not an answer about this one, and a
	// board that no longer records this lease is not this holder's to keep
	// looking alive.
	if liveness.Held && liveness.LeaseID != token.LeaseID {
		return boardclient.HolderLiveness{}, boardclient.ErrInvalidRequest
	}
	if snapshot.Lease == nil || snapshot.Lease.ID != token.LeaseID ||
		snapshot.Lease.Generation != token.Generation {
		return boardclient.HolderLiveness{}, boardclient.ErrStaleLease
	}
	// The two halves of a beat are one answer about one board, so a
	// liveness half reporting nobody holds a board whose snapshot still
	// carries this lease is not an answer to act on.
	if !beatHalvesAgree(snapshot, liveness) {
		return boardclient.HolderLiveness{}, boardclient.ErrInvalidRequest
	}
	return liveness, nil
}

// KeepAlive reports this holder alive for as long as ctx runs, on the interval
// the server hands back with each beat.
//
// A cancelled context is the normal end of a holder's work, so it returns nil.
// A lease that stopped being this caller's ends the loop with that error,
// because beating harder at a board somebody else now holds is exactly the
// claim the beat must never make. A transport error does not end it: the loop
// keeps the interval it last knew and reports again, since silence is already
// the server's answer for a holder it cannot hear from.
func (a *Agent) KeepAlive(ctx context.Context, token boardclient.LeaseToken) error {
	if a == nil || ctx == nil || !validHeartbeatToken(a, token) {
		return ErrInvalidAgent
	}
	if _, ok := a.client.(HeartbeatClient); !ok {
		return ErrInvalidAgent
	}
	interval := defaultHeartbeatInterval
	for {
		liveness, err := a.ReportAlive(ctx, token)
		switch {
		case err == nil:
			interval = beatInterval(liveness.Interval)
		case ctx.Err() != nil:
			return nil
		case settledBeat(err):
			return err
		}
		if err := waitBeat(ctx, interval); err != nil {
			return nil
		}
	}
}

func waitBeat(ctx context.Context, interval time.Duration) error {
	timer := time.NewTimer(interval)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}

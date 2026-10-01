package store

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5"
)

// Committing a heartbeat, which is the one board transition that records state
// and emits no event.
//
// Every other command owes an event for every version it advances, and the
// transition path is built around that: it writes the snapshot, projects the
// lease and queue, records any yield sample, then inserts the events and their
// audit rows. A heartbeat has none of the last part by design, so routing it
// down that path would either write nothing durable (leaving the beat in
// memory only, where a restart loses exactly the evidence a crashed holder is
// judged by) or manufacture an event the audited set does not want.
//
// So it gets its own write, and the cost of a second write path is a second
// way to change a board. That is what boardWriteFor and livenessOnly exist to
// close: the event-free path is reachable only for a command that declares
// itself event-free, and only for a snapshot whose sole difference is a beat
// that moved forward.

// boardWrite is what a reducer result owes the database.
type boardWrite uint8

const (
	writeNothing boardWrite = iota
	writeTransition
	writeLiveness
)

// boardWriteFor decides which write a reducer result earns, refusing any
// result that does not fit one of them. It is pure so the refusals can be
// tested without a database, which matters because these are the checks that
// stand between a reducer defect and a committed board.
func boardWriteFor(before, after board.Snapshot, command board.Command, events []board.Event) (boardWrite, error) {
	if len(events) > 0 {
		if after.Version != before.Version+1 {
			return writeNothing, fmt.Errorf("%w: reducer event without version advance", ErrConflict)
		}
		return writeTransition, nil
	}
	if after.Version == before.Version {
		return writeNothing, nil
	}
	if !board.EventFreeCommand(command) || after.Version != before.Version+1 {
		return writeNothing, fmt.Errorf("%w: reducer advanced version without events", ErrConflict)
	}
	if err := livenessOnly(before, after); err != nil {
		return writeNothing, err
	}
	return writeLiveness, nil
}

// livenessOnly refuses an event-free write that changed anything but the beat.
//
// The reducer already limits a heartbeat to the one field, so this is the
// same property derived a second time from the pair of snapshots rather than
// from trust in the command that produced them. It is the check that keeps
// "event-free" from becoming a way to move a board without a record: a phase
// change, a grant, a queue edit or a new deadline arriving through this path
// is refused here rather than committed silently.
func livenessOnly(before, after board.Snapshot) error {
	if before.Lease == nil || after.Lease == nil {
		return fmt.Errorf("%w: event-free board write without a lease", ErrConflict)
	}
	if !after.Lease.LastHeartbeatAt.After(before.Lease.LastHeartbeatAt) {
		return fmt.Errorf("%w: event-free board write recorded no later beat", ErrConflict)
	}
	asBefore := *after.Lease
	asBefore.LastHeartbeatAt = before.Lease.LastHeartbeatAt
	if asBefore != *before.Lease {
		return fmt.Errorf("%w: event-free board write changed the lease", ErrConflict)
	}
	if before.BoardID != after.BoardID || before.Phase != after.Phase ||
		before.Generation != after.Generation || before.AgentHighWater != after.AgentHighWater ||
		before.NextSequence != after.NextSequence || len(before.Queue) != len(after.Queue) {
		return fmt.Errorf("%w: event-free board write changed board state", ErrConflict)
	}
	for i := range before.Queue {
		if before.Queue[i] != after.Queue[i] {
			return fmt.Errorf("%w: event-free board write changed the queue", ErrConflict)
		}
	}
	return nil
}

// persistBoardLiveness commits a beat and nothing else.
//
// No projection: board_leases carries the columns SQL enforces uniqueness on
// and a beat changes none of them, so the lease row is already correct. No
// event row and no audit row: that is the whole point of the event-free path.
// The compare-and-set is the same one the transition path uses, so a beat
// still cannot overwrite a board another transaction moved underneath it.
func persistBoardLiveness(ctx context.Context, tx pgx.Tx, before, after board.Snapshot) error {
	encoded, err := json.Marshal(after)
	if err != nil {
		return fmt.Errorf("%w: encode board: %v", ErrUnavailable, err)
	}
	tag, err := tx.Exec(ctx, `UPDATE board_snapshots SET version=$2, state=$3,
		updated_at=clock_timestamp() WHERE board_id=$1 AND version=$4`,
		after.BoardID, int64(after.Version), encoded, int64(before.Version))
	if err != nil || tag.RowsAffected() != 1 {
		return fmt.Errorf("%w: board snapshot CAS: %v", ErrConflict, err)
	}
	return nil
}

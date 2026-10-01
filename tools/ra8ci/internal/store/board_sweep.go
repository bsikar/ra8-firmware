package store

import (
	"context"
	"fmt"
	"time"
)

// Finding the boards whose holder never came back.
//
// board.Apply expires a lease as its first act, before the command it was
// given, so a board that is asked anything at all after its deadline recovers
// on the spot. Nothing asks an idle board anything. TickBoard is the
// server-clock transition that exists for exactly this and has had no
// production caller since it landed: a holder that dies at minute two of an
// hour lease leaves the board held, its queue unserved and its recovery
// unrequested, until some unrelated request happens to arrive. On a bench
// that is quiet overnight, that is the whole night.
//
// This is the read behind the sweep. It never decides that a lease is over:
// it reports the rows whose recorded deadline has passed, and the reducer
// re-derives the same judgement under its own clock inside the transaction
// that acts on it.

// expiredLeasePage is the row bound of one read when the caller names none.
// Reclaiming is idempotent and the next pass is seconds away, so a long
// backlog is taken in bites rather than in one transaction-hungry sweep.
const expiredLeasePage = 100

// maxExpiredLeasePage is the largest page a caller may ask for. A pass holds
// one board lock at a time, so the ceiling bounds how long a single pass can
// hold the sweep behind it, not how much work it may ever do.
const maxExpiredLeasePage = 1000

// ExpiredBoardLease is one board whose live lease has outlived its deadline.
//
// Version is the board's snapshot version at read time, which is what the
// reclaiming tick applies under: if anything moved the board between the read
// and the tick, the compare-and-set refuses, and that refusal is the right
// answer because whatever moved it ran the same expiry first.
type ExpiredBoardLease struct {
	BoardID   string
	LeaseID   string
	Holder    string
	ExpiresAt time.Time
	Version   uint64
}

// expiredLeaseRow is one row as stored, before it is judged.
type expiredLeaseRow struct {
	BoardID   string
	LeaseID   string
	Holder    string
	ExpiresAt time.Time
	Version   int64
}

// expiredLeaseArgs shapes one read's arguments, or refuses it. Separated from
// the query so the refusals are testable without a database.
func expiredLeaseArgs(now time.Time, limit int) ([]any, error) {
	if now.IsZero() {
		return nil, fmt.Errorf("%w: expired lease read needs the current time", ErrInvalid)
	}
	if limit < 0 || limit > maxExpiredLeasePage {
		return nil, fmt.Errorf("%w: expired lease page size %d", ErrInvalid, limit)
	}
	if limit == 0 {
		limit = expiredLeasePage
	}
	return []any{now.UTC(), limit}, nil
}

// expiredLeaseFrom judges one stored row against the read that asked for it.
//
// The deadline is checked a second time here, on the value that came back,
// rather than trusted from the WHERE clause. This is the same derive-twice
// shape the rest of this seam uses: a row that is not actually expired must
// never reach a caller whose whole purpose is to act on expiry, and the loud
// refusal names the board instead of quietly reclaiming a live lease.
func expiredLeaseFrom(row expiredLeaseRow, now time.Time) (ExpiredBoardLease, error) {
	if row.BoardID == "" || row.LeaseID == "" || row.Holder == "" {
		return ExpiredBoardLease{}, fmt.Errorf("%w: stored lease identity", ErrConflict)
	}
	if row.ExpiresAt.IsZero() || row.ExpiresAt.After(now) {
		return ExpiredBoardLease{}, fmt.Errorf("%w: lease %s on board %s has not expired", ErrConflict, row.LeaseID, row.BoardID)
	}
	// A board holding a lease has been through at least one transition, so
	// its snapshot version is past zero. Version zero here would mean the
	// tick applies under an expectation the board cannot be at, and the
	// sweep would spin on a conflict it re-earns every pass.
	if row.Version <= 0 {
		return ExpiredBoardLease{}, fmt.Errorf("%w: board %s holds a lease at snapshot version %d", ErrConflict, row.BoardID, row.Version)
	}
	return ExpiredBoardLease{
		BoardID:   row.BoardID,
		LeaseID:   row.LeaseID,
		Holder:    row.Holder,
		ExpiresAt: row.ExpiresAt.UTC(),
		Version:   uint64(row.Version),
	}, nil
}

// ExpiredBoardLeases reads the boards whose live lease has passed its
// deadline, oldest deadline first, for the sweep to reclaim. A limit of zero
// takes the default page.
//
// It joins the snapshot because the tick needs the version, and a lease with
// no snapshot row is not reclaimable by this path at all: the reducer reads
// its state from the snapshot, so such a row is a projection defect for
// someone else to answer, not a board to tick.
func (s *Store) ExpiredBoardLeases(ctx context.Context, now time.Time, limit int) ([]ExpiredBoardLease, error) {
	if s == nil || s.pool == nil {
		return nil, fmt.Errorf("%w: store is nil", ErrUnavailable)
	}
	args, err := expiredLeaseArgs(now, limit)
	if err != nil {
		return nil, err
	}
	rows, err := s.pool.Query(ctx, `SELECT l.board_id, l.id::text, l.holder_id, l.expires_at, b.version
		FROM board_leases l
		JOIN board_snapshots b ON b.board_id = l.board_id
		WHERE l.state IN ('pending', 'active') AND l.expires_at <= $1
		ORDER BY l.expires_at, l.board_id LIMIT $2`, args...)
	if err != nil {
		return nil, fmt.Errorf("%w: expired lease query: %v", ErrUnavailable, err)
	}
	defer rows.Close()

	expired := make([]ExpiredBoardLease, 0, 8)
	for rows.Next() {
		var row expiredLeaseRow
		if err := rows.Scan(&row.BoardID, &row.LeaseID, &row.Holder, &row.ExpiresAt, &row.Version); err != nil {
			return nil, fmt.Errorf("%w: expired lease scan: %v", ErrUnavailable, err)
		}
		lease, err := expiredLeaseFrom(row, now.UTC())
		if err != nil {
			return nil, err
		}
		expired = append(expired, lease)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: expired lease rows: %v", ErrUnavailable, err)
	}
	return expired, nil
}

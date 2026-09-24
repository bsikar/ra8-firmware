package store

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Retaining the request-to-neutral measurement a yield leaves behind.
//
// The dynamic yield budget estimates over comparable history and nothing ever
// produced any, so every cohort sat below the sample floor and every estimate
// fell back to the task's declared bounds. board.YieldSampleFor has derived
// the sample from a committed transition since it landed and had no caller.
//
// This is that caller, and it sits inside the board transaction on purpose.
// before and events are exactly what YieldSampleFor takes, so the sample is
// written by the same transaction that made it true: a handoff cannot end and
// leave the history describing it to a later pass that may not run.
//
// Nothing here supplies the cohort. Both halves of the promise, the shown
// target and the cohort it was estimated over, are recorded on the lease when
// the yield is asked for, so the row is filed against the history the
// requester was actually quoted rather than against whatever the board is
// doing by the time it reaches neutral.

// yieldSampleRow is the sample as one database row.
type yieldSampleRow struct {
	LeaseID         string
	BoardID         string
	WaiterID        string
	Cohort          board.YieldCohort
	RequestedAt     time.Time
	NeutralAt       time.Time
	ExclusionReason string
	SafetyOverrun   bool
	ShownTarget     time.Duration
}

// yieldSampleRowFor shapes a derived sample into its row, or reports that the
// transition measured nothing. It is separated from the write so the shaping
// is testable without a database, which is the half that decides what the
// estimator later reads.
func yieldSampleRowFor(before board.Snapshot, events []board.Event) (yieldSampleRow, bool, error) {
	// A zero cohort and a zero target take whatever the lease recorded. The
	// store has no better source for either: it cannot re-derive a cohort
	// without describing work the promise never covered.
	sample, ok, err := board.YieldSampleFor(before, events, board.YieldCohort{}, 0)
	if err != nil || !ok {
		return yieldSampleRow{}, false, err
	}
	var shown time.Duration
	if before.Lease != nil {
		shown = before.Lease.HandoffTarget
	}
	row := yieldSampleRow{
		LeaseID:         sample.LeaseID,
		BoardID:         before.BoardID,
		WaiterID:        sample.WaiterID,
		Cohort:          sample.Cohort,
		RequestedAt:     sample.RequestedAt,
		NeutralAt:       sample.NeutralAt,
		ExclusionReason: sample.ExclusionReason,
		SafetyOverrun:   sample.SafetyOverrun,
		ShownTarget:     shown,
	}
	// The table's own CHECK says a row is a measurement or a censored row
	// naming why it is not one, never both and never neither. Refusing here
	// too keeps the failure a named conflict instead of a constraint
	// violation surfacing as an opaque unavailable.
	if row.NeutralAt.IsZero() == (row.ExclusionReason == "") {
		return yieldSampleRow{}, false, fmt.Errorf("%w: yield sample is neither measured nor censored", ErrConflict)
	}
	if row.LeaseID == "" || row.BoardID == "" || row.RequestedAt.IsZero() {
		return yieldSampleRow{}, false, fmt.Errorf("%w: yield sample identity", ErrConflict)
	}
	return row, true, nil
}

// recordYieldSample writes at most one sample for a committed transition.
//
// ON CONFLICT DO NOTHING, keyed on the lease: one lease yields at most one
// measured handoff, and a retried or replayed transition must not file a
// second row that the estimator would count twice.
func recordYieldSample(ctx context.Context, tx pgx.Tx, before board.Snapshot, events []board.Event) error {
	row, ok, err := yieldSampleRowFor(before, events)
	if err != nil {
		return err
	}
	if !ok {
		return nil
	}
	_, err = tx.Exec(ctx, `INSERT INTO board_yield_samples
		(lease_id,board_id,waiter_id,board_model,fixture_revision,task_name,catalog_digest,
		image_sha256,requested_at,neutral_at,exclusion_reason,safety_overrun,shown_target_ms)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)
		ON CONFLICT (lease_id) DO NOTHING`,
		row.LeaseID, row.BoardID, nullable(row.WaiterID), row.Cohort.BoardModel,
		row.Cohort.FixtureRevision, row.Cohort.TaskName, row.Cohort.CatalogDigest,
		row.Cohort.ImageSHA256, row.RequestedAt, nullTime(row.NeutralAt),
		nullableText(row.ExclusionReason), row.SafetyOverrun, row.ShownTarget.Milliseconds())
	if err != nil {
		return fmt.Errorf("%w: record yield sample: %v", ErrUnavailable, err)
	}
	return nil
}

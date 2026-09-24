package store

import (
	"context"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Reading back the history a yield estimate rests on.
//
// Samples have been written by the transaction that ends a handoff since
// migration 0023, and nothing ever read one: board.EstimateHandoff takes its
// samples from a caller, and every caller so far supplied its own. This is the
// read, and it is the last piece between a recorded handoff and the ETA the
// next requester is shown.
//
// Two decisions are deliberately NOT taken here. The estimator owns the
// staleness judgement (it measures age from neutral_at and reports what it
// dropped), and it owns which rows count. The read bounds the scan and hands
// back rows; it never pre-judges a cohort's history, because a row silently
// excluded by the read is one the provenance line shown to a person cannot
// account for.

// yieldHistoryPageSize is the row bound of one read. It matches the
// estimator's own bound, so a read can never hand back a page
// EstimateHandoff would refuse outright.
const yieldHistoryPageSize = board.MaxHandoffSamples

// yieldHistoryCutoff is the oldest request time a read returns.
//
// It is the estimator's sample age widened by the longest handoff the
// estimator will still count. The estimator measures age from neutral_at and
// this filter reads requested_at, so the widening is what keeps the two from
// disagreeing: a handoff requested just before the age boundary that reached
// neutral inside it is fresh history, and a narrower filter on requested_at
// would drop exactly the slow handoffs an ETA most needs to cover. Filtering
// on neutral_at directly would read past the cohort index, which orders on
// requested_at, and would also drop every censored row (it has no neutral
// time) from a count the requester is shown.
func yieldHistoryCutoff(now time.Time) time.Time {
	return now.Add(-(board.MaxHandoffSampleAge + board.MaxHandoffBound))
}

// yieldHistoryArgs shapes one read's arguments, or refuses it. Separated from
// the query so the refusals are testable without a database.
func yieldHistoryArgs(cohort board.YieldCohort, now time.Time) ([]any, error) {
	if err := board.ValidateYieldCohort(cohort); err != nil {
		return nil, err
	}
	if now.IsZero() {
		return nil, fmt.Errorf("%w: yield history needs the current time", ErrInvalid)
	}
	return []any{cohort.BoardID, cohort.BoardModel, cohort.FixtureRevision, cohort.TaskName,
		cohort.CatalogDigest, cohort.ImageSHA256, yieldHistoryCutoff(now), yieldHistoryPageSize}, nil
}

// yieldSampleFrom turns one stored row back into a sample, judged against the
// cohort the read asked for.
//
// The cohort check is the derive-twice shape the rest of this seam uses: the
// query selects one cohort by equality on all six columns, so a row arriving
// under a different one means the read and the row disagree about what is
// comparable. The estimator would skip such a row silently and count it
// toward nothing, which is the quiet answer. This is the loud one.
func yieldSampleFrom(row yieldSampleRow, requested board.YieldCohort) (board.YieldSample, error) {
	if row.Cohort != requested {
		return board.YieldSample{}, fmt.Errorf("%w: stored yield sample %s is outside the cohort read", ErrConflict, row.LeaseID)
	}
	if row.LeaseID == "" || row.RequestedAt.IsZero() {
		return board.YieldSample{}, fmt.Errorf("%w: stored yield sample identity", ErrConflict)
	}
	// The same either-or the table CHECKs and the recorder refuses. Reading
	// it back is where a row written by an older or a hand-edited path would
	// show up, and a row that is both would be counted as a measurement and
	// as a censored row at once.
	if row.NeutralAt.IsZero() == (row.ExclusionReason == "") {
		return board.YieldSample{}, fmt.Errorf("%w: stored yield sample %s is neither measured nor censored", ErrConflict, row.LeaseID)
	}
	if row.ExclusionReason != "" && row.SafetyOverrun {
		return board.YieldSample{}, fmt.Errorf("%w: censored yield sample %s claims a safety overrun", ErrConflict, row.LeaseID)
	}
	if !row.NeutralAt.IsZero() && row.NeutralAt.Before(row.RequestedAt) {
		return board.YieldSample{}, fmt.Errorf("%w: stored yield sample %s reaches neutral before its request", ErrConflict, row.LeaseID)
	}
	return board.YieldSample{
		Cohort:          row.Cohort,
		LeaseID:         row.LeaseID,
		WaiterID:        row.WaiterID,
		RequestedAt:     row.RequestedAt,
		NeutralAt:       row.NeutralAt,
		SafetyOverrun:   row.SafetyOverrun,
		ExclusionReason: row.ExclusionReason,
	}, nil
}

// YieldHistory reads the comparable history for one cohort, newest request
// first, for board.EstimateHandoff to estimate over.
func (s *Store) YieldHistory(ctx context.Context, cohort board.YieldCohort, now time.Time) ([]board.YieldSample, error) {
	args, err := yieldHistoryArgs(cohort, now)
	if err != nil {
		return nil, err
	}
	rows, err := s.pool.Query(ctx, `SELECT lease_id, board_id, COALESCE(waiter_id::text, ''),
		board_model, fixture_revision, task_name, catalog_digest, image_sha256,
		requested_at, neutral_at, COALESCE(exclusion_reason, ''), safety_overrun
		FROM board_yield_samples
		WHERE board_id = $1 AND board_model = $2 AND fixture_revision = $3
		AND task_name = $4 AND catalog_digest = $5 AND image_sha256 = $6
		AND requested_at >= $7
		ORDER BY requested_at DESC, lease_id LIMIT $8`, args...)
	if err != nil {
		return nil, fmt.Errorf("%w: yield history query: %v", ErrUnavailable, err)
	}
	defer rows.Close()

	samples := make([]board.YieldSample, 0, board.MinimumHandoffSamples)
	for rows.Next() {
		var row yieldSampleRow
		var neutral *time.Time
		if err := rows.Scan(&row.LeaseID, &row.BoardID, &row.WaiterID, &row.Cohort.BoardModel,
			&row.Cohort.FixtureRevision, &row.Cohort.TaskName, &row.Cohort.CatalogDigest,
			&row.Cohort.ImageSHA256, &row.RequestedAt, &neutral, &row.ExclusionReason,
			&row.SafetyOverrun); err != nil {
			return nil, fmt.Errorf("%w: yield history scan: %v", ErrUnavailable, err)
		}
		row.Cohort.BoardID = row.BoardID
		if neutral != nil {
			row.NeutralAt = neutral.UTC()
		}
		row.RequestedAt = row.RequestedAt.UTC()
		sample, err := yieldSampleFrom(row, cohort)
		if err != nil {
			return nil, err
		}
		samples = append(samples, sample)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: yield history rows: %v", ErrUnavailable, err)
	}
	return samples, nil
}

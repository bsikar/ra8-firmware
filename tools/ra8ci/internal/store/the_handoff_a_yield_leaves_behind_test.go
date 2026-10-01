//go:build integration

package store

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Filing the measurement a yield leaves behind.
//
// The dynamic yield budget estimates over comparable history, and nothing
// ever produced any: every cohort sat below the sample floor and every
// estimate fell back to the task's declared bounds. recordYieldSample is the
// write that ends that, and it sits inside the board transaction on purpose,
// so a handoff cannot end and leave the history describing it to a later
// pass that may not run.
//
// The shaping half is already held without a database by
// board_yield_samples_test.go. This is the write: what actually reaches the
// table, and what happens when the same transition is filed twice.

// yieldFiled is one stored sample, read back as the estimator would see it.
type yieldFiled struct {
	boardID   string
	waiterID  *string
	neutralAt *time.Time
	exclusion *string
	overrun   bool
	shownMS   int64
	taskName  string
	digest    string
}

func filedSamples(t *testing.T, ctx context.Context, pool *pgxpool.Pool, leaseID string) []yieldFiled {
	t.Helper()
	rows, err := pool.Query(ctx, `SELECT board_id,waiter_id,neutral_at,exclusion_reason,
		safety_overrun,shown_target_ms,task_name,catalog_digest
		FROM board_yield_samples WHERE lease_id=$1`, leaseID)
	if err != nil {
		t.Fatalf("reading filed samples: %v", err)
	}
	defer rows.Close()
	var filed []yieldFiled
	for rows.Next() {
		var one yieldFiled
		if err := rows.Scan(&one.boardID, &one.waiterID, &one.neutralAt, &one.exclusion,
			&one.overrun, &one.shownMS, &one.taskName, &one.digest); err != nil {
			t.Fatalf("scanning filed sample: %v", err)
		}
		filed = append(filed, one)
	}
	return filed
}

// fileSample runs the write the way the board transaction does, in its own
// transaction, and returns whatever it refused.
func fileSample(ctx context.Context, pool *pgxpool.Pool, before board.Snapshot, events []board.Event) error {
	tx, err := pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	if err := recordYieldSample(ctx, tx, before, events); err != nil {
		return err
	}
	return tx.Commit(ctx)
}

// yielded is a held board as it stands after a yield was asked for: the
// promise shown to the requester and the cohort it was estimated over are
// both recorded on the lease, which is where the sample takes them from.
func yielded(boardID, leaseID string, cohort board.YieldCohort,
	requestedAt time.Time, target time.Duration) board.Snapshot {
	return board.Snapshot{
		BoardID: boardID, Phase: board.YieldRequested, Generation: 3, Version: 9,
		Lease: &board.Lease{
			ID: leaseID, Holder: "ci", Class: board.ClassCI, Reason: "integration run",
			Generation: 3, GrantedAt: requestedAt.Add(-20 * time.Minute),
			ExpiresAt: requestedAt.Add(10 * time.Minute), RequestedDuration: 30 * time.Minute,
			YieldRequestedAt: requestedAt, HandoffTarget: target, HandoffCohort: cohort,
		},
	}
}

func TestIntegrationAMeasuredHandoffIsFiledOnceForItsLease(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	// A real board and a real lease, because the sample hangs off both.
	boardID, _, waiter, _ := leasedUnder(t, ctx, st, pool, time.Now().UTC().Truncate(time.Second))
	cohort := isolatedCohort(t)
	requestedAt := time.Now().UTC().Add(-5 * time.Minute).Truncate(time.Millisecond)
	before := yielded(boardID, waiter.LeaseID, cohort, requestedAt, 45*time.Second)
	neutral := requestedAt.Add(30 * time.Second)

	if err := fileSample(ctx, pool, before, []board.Event{{
		Kind: board.LeaseReleased, LeaseID: waiter.LeaseID, At: neutral, Actor: "ci"}}); err != nil {
		t.Fatalf("filing a measured handoff: %v", err)
	}

	filed := filedSamples(t, ctx, pool, waiter.LeaseID)
	if len(filed) != 1 {
		t.Fatalf("want one filed sample, got %d", len(filed))
	}
	one := filed[0]
	if one.boardID != boardID {
		t.Fatalf("sample filed against board %q", one.boardID)
	}
	if one.neutralAt == nil || !one.neutralAt.UTC().Equal(neutral) {
		t.Fatalf("neutral stamp is %v, want %v", one.neutralAt, neutral)
	}
	if one.exclusion != nil {
		t.Fatalf("a measured handoff was filed as censored: %q", *one.exclusion)
	}
	// The promise is stored as shown, not re-derived: an estimator comparing
	// the measurement against a target nobody was quoted is measuring noise.
	if one.shownMS != 45000 {
		t.Fatalf("shown target filed as %dms, want 45000", one.shownMS)
	}
	// The cohort decides which history this measurement joins, so it has to
	// be the lease's own, not whatever the board is doing now.
	if one.taskName != cohort.TaskName || one.digest != cohort.CatalogDigest {
		t.Fatalf("sample filed against cohort %q/%q", one.taskName, one.digest)
	}
	if one.waiterID != nil {
		t.Fatalf("an absent waiter was filed as %q rather than left null", *one.waiterID)
	}

	// A retried or replayed transition must not file a second row: one lease
	// yields at most one measured handoff, and a double row is a measurement
	// the estimator counts twice.
	replayed := neutral.Add(time.Minute)
	if err := fileSample(ctx, pool, before, []board.Event{{
		Kind: board.LeaseReleased, LeaseID: waiter.LeaseID, At: replayed, Actor: "ci"}}); err != nil {
		t.Fatalf("replaying the transition: %v", err)
	}
	after := filedSamples(t, ctx, pool, waiter.LeaseID)
	if len(after) != 1 {
		t.Fatalf("a replayed transition filed %d rows", len(after))
	}
	if after[0].neutralAt == nil || !after[0].neutralAt.UTC().Equal(neutral) {
		t.Fatalf("the replay overwrote the first measurement: %v", after[0].neutralAt)
	}
}

func TestIntegrationACensoredHandoffIsFiledWithItsReason(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	boardID, _, waiter, _ := leasedUnder(t, ctx, st, pool, time.Now().UTC().Truncate(time.Second))
	cohort := isolatedCohort(t)
	requestedAt := time.Now().UTC().Add(-5 * time.Minute).Truncate(time.Millisecond)
	before := yielded(boardID, waiter.LeaseID, cohort, requestedAt, 45*time.Second)

	// A lease that expired while a yield was outstanding measured nothing,
	// but the fact that it measured nothing is itself history: filing it
	// censored keeps the cohort's sample count honest instead of quietly
	// dropping the slow cases and flattering the estimate.
	if err := fileSample(ctx, pool, before, []board.Event{{
		Kind: board.LeaseExpired, LeaseID: waiter.LeaseID,
		At: requestedAt.Add(10 * time.Minute), Actor: "server"}}); err != nil {
		t.Fatalf("filing a censored handoff: %v", err)
	}

	filed := filedSamples(t, ctx, pool, waiter.LeaseID)
	if len(filed) != 1 {
		t.Fatalf("want one filed sample, got %d", len(filed))
	}
	if filed[0].neutralAt != nil {
		t.Fatalf("a censored row carries a neutral stamp: %v", filed[0].neutralAt)
	}
	if filed[0].exclusion == nil || *filed[0].exclusion != board.YieldExcludedExpired {
		t.Fatalf("censored row names reason %v, want %q", filed[0].exclusion, board.YieldExcludedExpired)
	}
}

func TestIntegrationATransitionThatMeasuresNothingFilesNothing(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	boardID, _, waiter, _ := leasedUnder(t, ctx, st, pool, time.Now().UTC().Truncate(time.Second))
	cohort := isolatedCohort(t)
	requestedAt := time.Now().UTC().Add(-5 * time.Minute).Truncate(time.Millisecond)

	// Nobody asked this board to yield, so its release is not a handoff and
	// writing a row for it would invent a measurement.
	quiet := yielded(boardID, waiter.LeaseID, cohort, requestedAt, 45*time.Second)
	quiet.Lease.YieldRequestedAt = time.Time{}
	if err := fileSample(ctx, pool, quiet, []board.Event{{
		Kind: board.LeaseReleased, LeaseID: waiter.LeaseID,
		At: requestedAt.Add(time.Second), Actor: "ci"}}); err != nil {
		t.Fatalf("a release with no yield outstanding was refused: %v", err)
	}
	if filed := filedSamples(t, ctx, pool, waiter.LeaseID); len(filed) != 0 {
		t.Fatalf("a release nobody asked for filed %d rows", len(filed))
	}

	// An outstanding yield with no event that ends it either way is still
	// running, and the row is written when it lands, not before.
	asking := yielded(boardID, waiter.LeaseID, cohort, requestedAt, 45*time.Second)
	if err := fileSample(ctx, pool, asking, []board.Event{{
		Kind: board.DrainStarted, LeaseID: waiter.LeaseID,
		At: requestedAt.Add(time.Second), Actor: "ci"}}); err != nil {
		t.Fatalf("an outstanding yield was refused: %v", err)
	}
	if filed := filedSamples(t, ctx, pool, waiter.LeaseID); len(filed) != 0 {
		t.Fatalf("an unfinished handoff filed %d rows", len(filed))
	}
}

//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5/pgxpool"
)

// The comparable history board.EstimateHandoff estimates over.
//
// YieldHistory is the only read between a stored handoff and an ETA a human is
// shown, and it carries three decisions no other seam repeats: which rows are
// one cohort, how far back the page reaches, and what happens when a stored
// stamp is not believable. The first two decide whether an estimate is built
// from comparable handoffs or from a neighbouring task's; the third decides
// whether an unbelievable row is skipped quietly (and counted toward nothing)
// or refused loudly. These pin all three against a real database, because the
// cohort equality is expressed in SQL and the clock judgement is expressed in
// Go, and a test over either half alone would miss their disagreement.

// isolatedCohort is a cohort nothing else in the shared database can collide
// with. The board ID is the first of the six equality columns, so a unique one
// isolates the whole read.
func isolatedCohort(t *testing.T) board.YieldCohort {
	t.Helper()
	return board.YieldCohort{
		BoardID:         "board-history-" + mustID(t),
		BoardModel:      "ra8p1-ek",
		FixtureRevision: "fixture-c",
		TaskName:        "hil-smoke",
		CatalogDigest:   "2411656a6225954d8f8b6b4a593a79b0b95ddd080c5040515cb0a227f65216e5",
	}
}

// handoff is one row to plant: a measurement when neutral is set, a censored
// row when exclusion is.
type handoff struct {
	cohort    board.YieldCohort
	requested time.Time
	neutral   time.Time
	exclusion string
	overrun   bool
}

// plantHandoffs writes each handoff as a board, an ended lease and a yield
// sample, and removes them again when the test ends. It returns the lease IDs
// in the order planted.
//
// The leases are 'ended' deliberately: board_one_live_lease_idx admits one
// live lease per board, and a historical handoff is over anyway. Cleanup runs
// in foreign-key order so a failed assertion cannot leave a board behind for
// the next test in this package to read.
func plantHandoffs(t *testing.T, ctx context.Context, pool *pgxpool.Pool, planted ...handoff) []string {
	t.Helper()
	ids := make([]string, 0, len(planted))
	generation := make(map[string]int64)
	for _, h := range planted {
		if _, err := pool.Exec(ctx, `INSERT INTO boards (id, generation, state, version)
			VALUES ($1, 0, 'available', 1) ON CONFLICT (id) DO NOTHING`, h.cohort.BoardID); err != nil {
			t.Fatalf("plant board %s: %v", h.cohort.BoardID, err)
		}
		generation[h.cohort.BoardID]++
		var leaseID string
		if err := pool.QueryRow(ctx, `INSERT INTO board_leases (id, board_id, generation, holder_id,
			priority, reason, requested_duration_seconds, granted_at, expires_at, ended_at,
			end_reason, state)
			VALUES (gen_random_uuid(), $1, $2, 'ci', 'ci', 'yield history fixture', 1800,
			$3::timestamptz, $3::timestamptz + interval '30 minutes',
			$3::timestamptz + interval '31 minutes', 'released', 'ended')
			RETURNING id::text`, h.cohort.BoardID, generation[h.cohort.BoardID],
			h.requested).Scan(&leaseID); err != nil {
			t.Fatalf("plant lease on %s: %v", h.cohort.BoardID, err)
		}
		var neutral, exclusion any
		if !h.neutral.IsZero() {
			neutral = h.neutral
		}
		if h.exclusion != "" {
			exclusion = h.exclusion
		}
		if _, err := pool.Exec(ctx, `INSERT INTO board_yield_samples (lease_id, board_id,
			board_model, fixture_revision, task_name, catalog_digest, image_sha256,
			requested_at, neutral_at, exclusion_reason, safety_overrun)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)`,
			leaseID, h.cohort.BoardID, h.cohort.BoardModel, h.cohort.FixtureRevision,
			h.cohort.TaskName, h.cohort.CatalogDigest, h.cohort.ImageSHA256,
			h.requested, neutral, exclusion, h.overrun); err != nil {
			t.Fatalf("plant sample %s: %v", leaseID, err)
		}
		ids = append(ids, leaseID)
	}
	t.Cleanup(func() {
		clean := context.Background()
		for _, id := range ids {
			if _, err := pool.Exec(clean, `DELETE FROM board_yield_samples WHERE lease_id = $1`, id); err != nil {
				t.Logf("cleanup sample %s: %v", id, err)
			}
			if _, err := pool.Exec(clean, `DELETE FROM board_leases WHERE id = $1`, id); err != nil {
				t.Logf("cleanup lease %s: %v", id, err)
			}
		}
		for boardID := range generation {
			if _, err := pool.Exec(clean, `DELETE FROM boards WHERE id = $1`, boardID); err != nil {
				t.Logf("cleanup board %s: %v", boardID, err)
			}
		}
	})
	return ids
}

// measured is a completed handoff requested ago and neutral after latency.
func measured(cohort board.YieldCohort, now time.Time, ago, latency time.Duration) handoff {
	requested := now.Add(-ago)
	return handoff{cohort: cohort, requested: requested, neutral: requested.Add(latency)}
}

func TestIntegrationYieldHistoryReadsOneCohortNewestFirst(t *testing.T) {
	st, pool := integrationStore(t)
	ctx := context.Background()
	now := time.Now().UTC()
	cohort := isolatedCohort(t)

	// Planted oldest first, so a read that preserved insertion order would
	// hand the estimator the stalest handoff as the most recent one.
	ids := plantHandoffs(t, ctx, pool,
		measured(cohort, now, 4*time.Hour, 90*time.Second),
		measured(cohort, now, 3*time.Hour, 70*time.Second),
		measured(cohort, now, 2*time.Hour, 50*time.Second),
		measured(cohort, now, 1*time.Hour, 30*time.Second),
	)

	samples, err := st.YieldHistory(ctx, cohort, now)
	if err != nil {
		t.Fatalf("yield history: %v", err)
	}
	if len(samples) != 4 {
		t.Fatalf("read %d samples, want the 4 planted", len(samples))
	}
	want := []string{ids[3], ids[2], ids[1], ids[0]}
	for i, sample := range samples {
		if sample.LeaseID != want[i] {
			t.Fatalf("sample %d is %s, want %s: the page is not newest-request-first",
				i, sample.LeaseID, want[i])
		}
		if sample.Cohort != cohort {
			t.Fatalf("sample %d carries cohort %+v, want %+v", i, sample.Cohort, cohort)
		}
		if !sample.Completed() {
			t.Fatalf("sample %d is not a completed handoff, want every planted measurement usable", i)
		}
		if sample.RequestedAt.Location() != time.UTC || sample.NeutralAt.Location() != time.UTC {
			t.Fatalf("sample %d stamps are not UTC: %v / %v", i, sample.RequestedAt.Location(),
				sample.NeutralAt.Location())
		}
	}
	// The estimator measures latency off these two stamps, so a round trip
	// that lost the gap would estimate over zeroes.
	if gap := samples[0].NeutralAt.Sub(samples[0].RequestedAt); gap != 30*time.Second {
		t.Fatalf("newest handoff measured %v, want 30s", gap)
	}
}

func TestIntegrationYieldHistoryBreaksATieOnTheLeaseID(t *testing.T) {
	st, pool := integrationStore(t)
	ctx := context.Background()
	now := time.Now().UTC()
	cohort := isolatedCohort(t)

	// Two handoffs requested at the same instant. Without the second ORDER BY
	// key their order is whatever the scan happens to produce, and a page
	// that reshuffles between reads makes an ETA jitter for no reason.
	at := now.Add(-90 * time.Minute)
	plantHandoffs(t, ctx, pool,
		handoff{cohort: cohort, requested: at, neutral: at.Add(time.Minute)},
		handoff{cohort: cohort, requested: at, neutral: at.Add(2 * time.Minute)},
	)

	first, err := st.YieldHistory(ctx, cohort, now)
	if err != nil {
		t.Fatalf("yield history: %v", err)
	}
	if len(first) != 2 {
		t.Fatalf("read %d samples, want 2", len(first))
	}
	if first[0].LeaseID >= first[1].LeaseID {
		t.Fatalf("tied requests came back %s then %s, want ascending lease ID",
			first[0].LeaseID, first[1].LeaseID)
	}
	second, err := st.YieldHistory(ctx, cohort, now)
	if err != nil {
		t.Fatalf("second yield history: %v", err)
	}
	for i := range first {
		if first[i].LeaseID != second[i].LeaseID {
			t.Fatalf("the same read reshuffled tied requests: %s then %s",
				first[i].LeaseID, second[i].LeaseID)
		}
	}
}

func TestIntegrationYieldHistoryKeepsNeighbouringCohortsApart(t *testing.T) {
	st, pool := integrationStore(t)
	ctx := context.Background()
	now := time.Now().UTC()
	cohort := isolatedCohort(t)

	// One neighbour per equality column, each differing in exactly that one.
	// A read that dropped any column from its WHERE clause would borrow a
	// handoff measured on different hardware, a different fixture, a
	// different task or a different image and call it comparable.
	otherBoard := cohort
	otherBoard.BoardID = "board-history-" + mustID(t)
	otherModel := cohort
	otherModel.BoardModel = "ra8m1-ek"
	otherFixture := cohort
	otherFixture.FixtureRevision = "fixture-d"
	otherTask := cohort
	otherTask.TaskName = "hil-soak"
	otherDigest := cohort
	otherDigest.CatalogDigest = "0000000000000000000000000000000000000000000000000000000000000000"
	// The imageless cohort must not borrow an imaged task's history, which is
	// the one neighbour an "empty means any" reading would wrongly admit.
	otherImage := cohort
	otherImage.ImageSHA256 = "1111111111111111111111111111111111111111111111111111111111111111"

	ids := plantHandoffs(t, ctx, pool,
		measured(cohort, now, time.Hour, 40*time.Second),
		measured(otherBoard, now, 30*time.Minute, 40*time.Second),
		measured(otherModel, now, 30*time.Minute, 40*time.Second),
		measured(otherFixture, now, 30*time.Minute, 40*time.Second),
		measured(otherTask, now, 30*time.Minute, 40*time.Second),
		measured(otherDigest, now, 30*time.Minute, 40*time.Second),
		measured(otherImage, now, 30*time.Minute, 40*time.Second),
	)

	samples, err := st.YieldHistory(ctx, cohort, now)
	if err != nil {
		t.Fatalf("yield history: %v", err)
	}
	if len(samples) != 1 || samples[0].LeaseID != ids[0] {
		t.Fatalf("read %d samples %v, want only the one in cohort (%s)",
			len(samples), leaseIDsOf(samples), ids[0])
	}

	// And the neighbour reads its own history rather than nothing, so the
	// isolation above is the WHERE clause working, not an empty table.
	theirs, err := st.YieldHistory(ctx, otherImage, now)
	if err != nil {
		t.Fatalf("neighbour yield history: %v", err)
	}
	if len(theirs) != 1 || theirs[0].LeaseID != ids[6] {
		t.Fatalf("neighbour read %d samples %v, want its own one (%s)",
			len(theirs), leaseIDsOf(theirs), ids[6])
	}
}

func TestIntegrationYieldHistoryBoundsHowFarBackItReaches(t *testing.T) {
	st, pool := integrationStore(t)
	ctx := context.Background()
	now := time.Now().UTC()
	cohort := isolatedCohort(t)

	// The cutoff is the estimator's sample age widened by the longest handoff
	// it will still count, so a handoff requested just inside it is fresh
	// history even though its request is older than the age bound alone.
	window := board.MaxHandoffSampleAge + board.MaxHandoffBound
	ids := plantHandoffs(t, ctx, pool,
		handoff{cohort: cohort, requested: now.Add(-window + time.Minute),
			neutral: now.Add(-window + 2*time.Minute)},
		handoff{cohort: cohort, requested: now.Add(-window - time.Minute),
			neutral: now.Add(-window)},
	)

	samples, err := st.YieldHistory(ctx, cohort, now)
	if err != nil {
		t.Fatalf("yield history: %v", err)
	}
	if len(samples) != 1 || samples[0].LeaseID != ids[0] {
		t.Fatalf("read %d samples %v, want only the one inside the %v window (%s)",
			len(samples), leaseIDsOf(samples), window, ids[0])
	}
}

func TestIntegrationYieldHistoryCarriesACensoredRow(t *testing.T) {
	st, pool := integrationStore(t)
	ctx := context.Background()
	now := time.Now().UTC()
	cohort := isolatedCohort(t)

	// A censored row is retained and counted toward the sample floor the
	// requester is shown, so it has to survive the read with its reason and
	// without a latency. An overrun is a claim about a measured handoff, so
	// it rides the measurement and not the censored row.
	requested := now.Add(-time.Hour)
	ids := plantHandoffs(t, ctx, pool,
		handoff{cohort: cohort, requested: requested, exclusion: "lease expired"},
		handoff{cohort: cohort, requested: requested.Add(time.Minute),
			neutral: requested.Add(3 * time.Minute), overrun: true},
	)

	samples, err := st.YieldHistory(ctx, cohort, now)
	if err != nil {
		t.Fatalf("yield history: %v", err)
	}
	if len(samples) != 2 {
		t.Fatalf("read %d samples, want both the measurement and the censored row", len(samples))
	}
	overran, censored := samples[0], samples[1]
	if overran.LeaseID != ids[1] || censored.LeaseID != ids[0] {
		t.Fatalf("read %v, want %s then %s", leaseIDsOf(samples), ids[1], ids[0])
	}
	if !overran.Completed() || !overran.SafetyOverrun {
		t.Fatalf("measured handoff came back completed=%v overrun=%v, want both true",
			overran.Completed(), overran.SafetyOverrun)
	}
	if censored.Completed() {
		t.Fatalf("censored row came back as a completed handoff")
	}
	if censored.ExclusionReason != "lease expired" {
		t.Fatalf("censored row reason is %q, want %q", censored.ExclusionReason, "lease expired")
	}
	if !censored.NeutralAt.IsZero() {
		t.Fatalf("censored row carries neutral %v, want no measurement", censored.NeutralAt)
	}
	if censored.SafetyOverrun {
		t.Fatalf("censored row claims a safety overrun")
	}
}

func TestIntegrationYieldHistoryRefusesAStoredStampAheadOfTheClock(t *testing.T) {
	st, pool := integrationStore(t)
	ctx := context.Background()
	now := time.Now().UTC()

	// requested_at is the ORDER BY key and the cutoff bounds only the old
	// side, so a row stamped ahead of this host holds the head of every page
	// of its cohort until the clock catches up. Skipping it quietly would
	// count it toward nothing while it displaces real history; the read
	// refuses instead, and names the lease an operator has to look at.
	for _, c := range []struct {
		name string
		row  func(cohort board.YieldCohort) handoff
	}{
		{"requested", func(cohort board.YieldCohort) handoff {
			at := now.Add(board.MaxClockOffset + time.Hour)
			return handoff{cohort: cohort, requested: at, neutral: at.Add(time.Minute)}
		}},
		{"neutral", func(cohort board.YieldCohort) handoff {
			return handoff{cohort: cohort, requested: now.Add(-time.Minute),
				neutral: now.Add(board.MaxClockOffset + time.Hour)}
		}},
	} {
		t.Run(c.name, func(t *testing.T) {
			cohort := isolatedCohort(t)
			ids := plantHandoffs(t, ctx, pool,
				measured(cohort, now, time.Hour, 40*time.Second), c.row(cohort))

			samples, err := st.YieldHistory(ctx, cohort, now)
			if !errors.Is(err, ErrConflict) {
				t.Fatalf("read %d samples err=%v, want ErrConflict for the %s stamp",
					len(samples), err, c.name)
			}
			if samples != nil {
				t.Fatalf("a refused read handed back %v, want no page at all", leaseIDsOf(samples))
			}
			if !strings.Contains(err.Error(), ids[1]) {
				t.Fatalf("refusal %q does not name lease %s", err, ids[1])
			}
		})
	}
}

func TestYieldHistoryRefusesAnIncoherentRead(t *testing.T) {
	st, _ := integrationStore(t)
	ctx := context.Background()
	now := time.Now().UTC()
	whole := isolatedCohort(t)

	// Shaped before a connection is spent, so an unanswerable read costs the
	// pool nothing. The five required columns are refused one at a time
	// rather than as a single empty cohort, because a read missing any one of
	// them would silently widen to every value of it.
	noBoard := whole
	noBoard.BoardID = ""
	noModel := whole
	noModel.BoardModel = ""
	noFixture := whole
	noFixture.FixtureRevision = ""
	noTask := whole
	noTask.TaskName = ""
	noDigest := whole
	noDigest.CatalogDigest = ""

	for _, c := range []struct {
		name   string
		cohort board.YieldCohort
		now    time.Time
	}{
		{"no board", noBoard, now},
		{"no model", noModel, now},
		{"no fixture revision", noFixture, now},
		{"no task", noTask, now},
		{"no catalog digest", noDigest, now},
		{"no cohort at all", board.YieldCohort{}, now},
		{"no clock", whole, time.Time{}},
	} {
		t.Run(c.name, func(t *testing.T) {
			samples, err := st.YieldHistory(ctx, c.cohort, c.now)
			// Two error types carry this, by design rather than by accident:
			// the cohort is judged by board.ValidateYieldCohort and arrives as
			// a *board.Error with code InvalidArgument, while the clock is the
			// store's own ErrInvalid. The server's writeBoardError maps the
			// first to 400 and the ErrInvalid path reaches the same status, so
			// what matters to a caller is that the read is refused as an
			// invalid argument. Asserting the disjunction pins that without
			// freezing which half of the seam raises it.
			if !board.IsCode(err, board.InvalidArgument) && !errors.Is(err, ErrInvalid) {
				t.Fatalf("read %d samples err=%v, want the read refused as an invalid argument",
					len(samples), err)
			}
			if samples != nil {
				t.Fatalf("a refused read handed back %d samples", len(samples))
			}
		})
	}
}

// leaseIDsOf names a page in a failure message.
func leaseIDsOf(samples []board.YieldSample) []string {
	ids := make([]string, 0, len(samples))
	for _, sample := range samples {
		ids = append(ids, sample.LeaseID)
	}
	return ids
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"net/http"
	"strconv"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The two segment doors fence a board agent's work between a begin and a
// finish. board_segments_test.go already holds the happy path of each and
// one unbounded refusal; this takes the rest of the front: the optional
// capability, the segment ID a finish is judged on, and every argument that
// must hold before the store is asked to fence anything.

// segmentRefusingStore carries the durable segment capability and refuses on
// demand, and counts what it was asked so a refused request can be shown
// never to have reached it.
type segmentRefusingStore struct {
	*fakeBoardStore
	begins    int
	finishes  int
	beginErr  error
	finishErr error
	segment   store.BoardSegment
}

func (s *segmentRefusingStore) BeginBoardSegment(_ context.Context, _ store.BoardActor, _ uint64, token board.Token, attemptID, key string, _, _ time.Duration) (store.BoardSegment, error) {
	s.begins++
	if s.beginErr != nil {
		return store.BoardSegment{}, s.beginErr
	}
	s.segment = store.BoardSegment{ID: boardTestProofID, BoardID: token.BoardID, LeaseID: token.LeaseID,
		Generation: token.Generation, AttemptID: attemptID, Key: key}
	return s.segment, nil
}

func (s *segmentRefusingStore) FinishBoardSegment(_ context.Context, _ store.BoardActor, _ string, _ board.Token, _, _ string) error {
	s.finishes++
	return s.finishErr
}

func newSegmentStore() *segmentRefusingStore {
	return &segmentRefusingStore{fakeBoardStore: &fakeBoardStore{}}
}

func segmentPlane(t *testing.T, st BoardStore) *http.ServeMux {
	t.Helper()
	mux := http.NewServeMux()
	if err := RegisterBoardRoutes(mux, st, nil, "bsikar/ra8-firmware"); err != nil {
		t.Fatal(err)
	}
	return mux
}

const beginPath = "/v1/boards/ek-ra8d2/segments/begin"

func finishPath(segmentID string) string {
	return "/v1/boards/ek-ra8d2/segments/" + segmentID + "/finish"
}

func aSegment(leaseID, attemptID string, bound, margin int64) string {
	return `{"expected_version":12,"lease_id":"` + leaseID + `","generation":5,"attempt_id":"` +
		attemptID + `","key":"flash","bound_milliseconds":` + strconv.FormatInt(bound, 10) +
		`,"recovery_margin_ms":` + strconv.FormatInt(margin, 10) + `}`
}

func aResult(outcome string) string {
	return `{"lease_id":"` + boardTestLeaseID + `","generation":5,"attempt_id":"` +
		boardTestProofID + `","outcome":"` + outcome + `"}`
}

// A plane whose store cannot fence segments says so on both doors rather
// than accepting work it cannot bound.
func TestSegmentDoorsNeedADurableStore(t *testing.T) {
	plane := segmentPlane(t, &fakeBoardStore{})

	for name, asked := range map[string]answered{
		"begin":  askedOfTheBoard(t, plane, beginPath, "application/json", aSegment(boardTestLeaseID, boardTestProofID, 25000, 3000)),
		"finish": askedOfTheBoard(t, plane, finishPath(boardTestProofID), "application/json", aResult("completed")),
	} {
		if asked.status != http.StatusServiceUnavailable ||
			asked.body["detail"] != "durable board segments are not configured" {
			t.Fatalf("%s answered %d %+v", name, asked.status, asked.body)
		}
	}
}

// TestSegmentBeginJudgesItsBoundsBeforeTheStore walks the begin door's
// arguments. The bound is the one that matters: a segment with no bound, or
// one a day long, is not a fence at all, and the door refuses both rather
// than letting the store hold a lease open indefinitely.
func TestSegmentBeginJudgesItsBoundsBeforeTheStore(t *testing.T) {
	const aDay = int64(24 * 60 * 60 * 1000)

	for name, refused := range map[string]struct {
		contentType string
		body        string
		status      int
		detail      string
	}{
		"no content type": {
			body:   aSegment(boardTestLeaseID, boardTestProofID, 25000, 3000),
			status: http.StatusUnsupportedMediaType, detail: "content type must be application/json",
		},
		"a request that does not parse": {
			contentType: "application/json", body: `{"key":`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a field this door does not know": {
			contentType: "application/json", body: `{"key":"flash","rack":"2"}`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a lease that is not an ID": {
			contentType: "application/json", body: aSegment("lease-1", boardTestProofID, 25000, 3000),
			status: http.StatusBadRequest, detail: "invalid board segment request",
		},
		"an attempt that is not an ID": {
			contentType: "application/json", body: aSegment(boardTestLeaseID, "attempt-1", 25000, 3000),
			status: http.StatusBadRequest, detail: "invalid board segment request",
		},
		"generation zero": {
			contentType: "application/json",
			body: `{"expected_version":12,"lease_id":"` + boardTestLeaseID + `","generation":0,"attempt_id":"` +
				boardTestProofID + `","key":"flash","bound_milliseconds":25000,"recovery_margin_ms":0}`,
			status: http.StatusBadRequest, detail: "invalid board segment request",
		},
		"no key": {
			contentType: "application/json",
			body: `{"expected_version":12,"lease_id":"` + boardTestLeaseID + `","generation":5,"attempt_id":"` +
				boardTestProofID + `","key":"","bound_milliseconds":25000,"recovery_margin_ms":0}`,
			status: http.StatusBadRequest, detail: "invalid board segment request",
		},
		"a negative bound": {
			contentType: "application/json", body: aSegment(boardTestLeaseID, boardTestProofID, -1, 0),
			status: http.StatusBadRequest, detail: "invalid board segment request",
		},
		"a bound past a day": {
			contentType: "application/json", body: aSegment(boardTestLeaseID, boardTestProofID, aDay+1, 0),
			status: http.StatusBadRequest, detail: "invalid board segment request",
		},
		"a negative recovery margin": {
			contentType: "application/json", body: aSegment(boardTestLeaseID, boardTestProofID, 25000, -1),
			status: http.StatusBadRequest, detail: "invalid board segment request",
		},
		"a recovery margin past a day": {
			contentType: "application/json", body: aSegment(boardTestLeaseID, boardTestProofID, 25000, aDay+1),
			status: http.StatusBadRequest, detail: "invalid board segment request",
		},
	} {
		st := newSegmentStore()
		result := askedOfTheBoard(t, segmentPlane(t, st), beginPath, refused.contentType, refused.body)
		if result.status != refused.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refused.status)
		}
		if result.body["detail"] != refused.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, refused.detail)
		}
		if st.begins != 0 {
			t.Fatalf("%s was carried into the store", name)
		}
	}
}

// Both bounds are inclusive at a day, which is the pair of the refusals
// above: the door draws its line at exactly 24 hours rather than somewhere
// near it.
func TestSegmentBeginAcceptsExactlyADay(t *testing.T) {
	const aDay = int64(24 * 60 * 60 * 1000)
	st := newSegmentStore()

	result := askedOfTheBoard(t, segmentPlane(t, st), beginPath, "application/json",
		aSegment(boardTestLeaseID, boardTestProofID, aDay, aDay))

	if result.status != http.StatusCreated {
		t.Fatalf("a day-long segment answered %d %+v", result.status, result.body)
	}
	if st.begins != 1 {
		t.Fatalf("the store was asked %d times, want once", st.begins)
	}
	if result.body["id"] != boardTestProofID || result.body["key"] != "flash" {
		t.Fatalf("the segment was rewritten on the way out: %+v", result.body)
	}
}

// TestSegmentFinishJudgesItsResultBeforeTheStore walks the finish door. The
// outcome is a closed set of three: anything else is refused here rather
// than written into the segment's history.
func TestSegmentFinishJudgesItsResultBeforeTheStore(t *testing.T) {
	for name, refused := range map[string]struct {
		segmentID   string
		contentType string
		body        string
		status      int
		detail      string
	}{
		"a segment ID that is not a UUID": {
			segmentID: "segment-1", contentType: "application/json", body: aResult("completed"),
			status: http.StatusBadRequest, detail: "invalid segment ID",
		},
		"no content type": {
			segmentID: boardTestProofID, body: aResult("completed"),
			status: http.StatusUnsupportedMediaType, detail: "content type must be application/json",
		},
		"a result that does not parse": {
			segmentID: boardTestProofID, contentType: "application/json", body: `{"outcome":`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a field this door does not know": {
			segmentID: boardTestProofID, contentType: "application/json", body: `{"outcome":"completed","notes":"fine"}`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a lease that is not an ID": {
			segmentID: boardTestProofID, contentType: "application/json",
			body:   `{"lease_id":"lease-1","generation":5,"attempt_id":"` + boardTestProofID + `","outcome":"completed"}`,
			status: http.StatusBadRequest, detail: "invalid board segment result",
		},
		"generation zero": {
			segmentID: boardTestProofID, contentType: "application/json",
			body: `{"lease_id":"` + boardTestLeaseID + `","generation":0,"attempt_id":"` + boardTestProofID +
				`","outcome":"completed"}`,
			status: http.StatusBadRequest, detail: "invalid board segment result",
		},
		"an outcome nobody declared": {
			segmentID: boardTestProofID, contentType: "application/json", body: aResult("cancelled"),
			status: http.StatusBadRequest, detail: "invalid board segment result",
		},
		"an outcome that is shouted": {
			segmentID: boardTestProofID, contentType: "application/json", body: aResult("COMPLETED"),
			status: http.StatusBadRequest, detail: "invalid board segment result",
		},
		"no outcome at all": {
			segmentID: boardTestProofID, contentType: "application/json", body: aResult(""),
			status: http.StatusBadRequest, detail: "invalid board segment result",
		},
	} {
		st := newSegmentStore()
		result := askedOfTheBoard(t, segmentPlane(t, st), finishPath(refused.segmentID), refused.contentType, refused.body)
		if result.status != refused.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refused.status)
		}
		if result.body["detail"] != refused.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, refused.detail)
		}
		if st.finishes != 0 {
			t.Fatalf("%s was carried into the store", name)
		}
	}
}

func TestSegmentFinishTakesAllThreeOutcomes(t *testing.T) {
	for _, outcome := range []string{"completed", "failed", "yielded"} {
		st := newSegmentStore()
		result := askedOfTheBoard(t, segmentPlane(t, st), finishPath(boardTestProofID), "application/json", aResult(outcome))
		if result.status != http.StatusOK {
			t.Fatalf("%q answered %d %+v", outcome, result.status, result.body)
		}
		if result.body["segment_id"] != boardTestProofID || result.body["outcome"] != outcome {
			t.Fatalf("%q answered %+v", outcome, result.body)
		}
		if st.finishes != 1 {
			t.Fatalf("%q reached the store %d times", outcome, st.finishes)
		}
	}
}

// The segment ID is judged as any UUID, while the lease and attempt are held
// to the tighter ID shape. A plain version 4 UUID is therefore a segment but
// would not be a lease, and that asymmetry is deliberate: segment IDs come
// from the store, lease IDs from the fencing token.
func TestSegmentFinishTakesAnyUUIDAsTheSegment(t *testing.T) {
	const version4 = "f47ac10b-58cc-4372-a567-0e02b2c3d479"
	if store.ValidID(version4) {
		t.Fatalf("%s was meant to be a UUID that is not a valid ID", version4)
	}

	st := newSegmentStore()
	result := askedOfTheBoard(t, segmentPlane(t, st), finishPath(version4), "application/json", aResult("completed"))

	if result.status != http.StatusOK || result.body["segment_id"] != version4 {
		t.Fatalf("a version 4 segment answered %d %+v", result.status, result.body)
	}
}

// A store that refuses is reported in the board's own wording on both doors.
func TestSegmentDoorsReportAStoresRefusal(t *testing.T) {
	for name, refusal := range map[string]struct {
		err    error
		status int
		detail string
	}{
		"a denied agent":       {err: store.ErrDenied, status: http.StatusNotFound, detail: "board not found or access denied"},
		"a board that is gone": {err: store.ErrNotFound, status: http.StatusNotFound, detail: "board not found"},
		"a store that is down": {err: store.ErrUnavailable, status: http.StatusServiceUnavailable},
	} {
		begun := newSegmentStore()
		begun.beginErr = refusal.err
		beginResult := askedOfTheBoard(t, segmentPlane(t, begun), beginPath, "application/json",
			aSegment(boardTestLeaseID, boardTestProofID, 25000, 3000))
		if beginResult.status != refusal.status {
			t.Fatalf("begin with %s: status = %d, want %d", name, beginResult.status, refusal.status)
		}

		finished := newSegmentStore()
		finished.finishErr = refusal.err
		finishResult := askedOfTheBoard(t, segmentPlane(t, finished), finishPath(boardTestProofID),
			"application/json", aResult("completed"))
		if finishResult.status != refusal.status {
			t.Fatalf("finish with %s: status = %d, want %d", name, finishResult.status, refusal.status)
		}
		if refusal.detail != "" && finishResult.body["detail"] != refusal.detail {
			t.Fatalf("finish with %s answered %+v", name, finishResult.body)
		}
	}
}

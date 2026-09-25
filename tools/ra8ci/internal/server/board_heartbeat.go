package server

import (
	"net/http"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Reporting that a board holder is still there, over HTTP.
//
// The lease invariant asks every lease to carry a requested duration, absolute
// expiry, heartbeat, holder identity, reason, and generation token. The board
// state machine grew the heartbeat half and nothing could reach it: the store
// answered "unsupported board command" for the command, and no route issued
// it. A holder that crashed at minute two of an hour lease was therefore
// indistinguishable from one working quietly, and an unavailable agent had to
// be declared from outside the state machine with no recorded evidence behind
// it.
//
// This is that door. It deliberately reports rather than decides: the response
// says when the holder was last seen and when it will start reading as
// overdue, and nothing here ends a lease. Silence waits for expiry and then a
// reviewed recovery sequence, which is the whole reason a beat is safe to
// accept from the holder itself.

// defaultHolderHeartbeatInterval is the reporting interval a deployment gets
// when it configures none. Three missed beats is three minutes, short enough
// to be read as a crash well inside the shortest class lifetime and long
// enough that a slow request is not one.
const defaultHolderHeartbeatInterval = time.Minute

type heartbeatRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	Generation      uint64 `json:"generation"`
}

func (h *boardHTTP) heartbeat(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.heartbeat")
	if !ok {
		return
	}
	leaseID := r.PathValue("lease_id")
	var req heartbeatRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(leaseID) || req.Generation == 0 {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid heartbeat", false)
		return
	}
	// One clock for the beat and the report, so the silence the holder is
	// told about is measured from the beat this request just recorded and
	// not from an instant slightly after it.
	now := time.Now().UTC()
	snapshot, events, err := h.store.ApplyBoardCommand(r.Context(), actor,
		board.HolderHeartbeat{LeaseID: leaseID, Generation: req.Generation},
		req.ExpectedVersion, nil, h.verifier, now)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	liveness, err := board.ObserveHolderLiveness(snapshot, now, h.heartbeatInterval)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Snapshot board.Snapshot     `json:"snapshot"`
		Events   []board.Event      `json:"events"`
		Liveness holderLivenessView `json:"liveness"`
	}{Snapshot: snapshot, Events: events, Liveness: newHolderLivenessView(liveness)})
}

// holderLivenessView is the liveness as the holder reads it.
//
// Durations are seconds for the same reason the yield plan states them that
// way: this is an answer a person or an agent loop reads, not state a client
// round-trips. NextBeatBy is the useful number, because it is the only one a
// holder can act on: report before it, or start reading as overdue.
type holderLivenessView struct {
	Held            bool       `json:"held"`
	LeaseID         string     `json:"lease_id,omitempty"`
	Holder          string     `json:"holder,omitempty"`
	LastSeenAt      *time.Time `json:"last_seen_at"`
	Beat            bool       `json:"beat"`
	SilenceSeconds  float64    `json:"silence_seconds"`
	IntervalSeconds float64    `json:"interval_seconds"`
	NextBeatBy      *time.Time `json:"next_beat_by"`
	Overdue         bool       `json:"overdue"`
	ExpiresAt       *time.Time `json:"expires_at"`
	Explain         string     `json:"explain"`
}

func newHolderLivenessView(l board.HolderLiveness) holderLivenessView {
	view := holderLivenessView{
		Held:            l.Held,
		LeaseID:         l.LeaseID,
		Holder:          l.Holder,
		Beat:            l.Beat,
		SilenceSeconds:  l.Silence.Seconds(),
		IntervalSeconds: l.Interval.Seconds(),
		Overdue:         l.Overdue,
		Explain:         l.Explain(),
	}
	if !l.Held {
		return view
	}
	// A time that exists is sent as a time; one that does not is null
	// rather than the zero instant, which reads as a date in year one a
	// client would have to know to special-case.
	if !l.LastSeenAt.IsZero() {
		seen := l.LastSeenAt
		view.LastSeenAt = &seen
		due := seen.Add(time.Duration(board.HeartbeatGraceBeats) * l.Interval)
		view.NextBeatBy = &due
	}
	if !l.ExpiresAt.IsZero() {
		expires := l.ExpiresAt
		view.ExpiresAt = &expires
	}
	return view
}

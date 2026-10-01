package server

import (
	"context"
	"net/http"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"github.com/google/uuid"
)

type durableBoardSegments interface {
	BeginBoardSegment(ctx context.Context, actor store.BoardActor, expectedVersion uint64, token board.Token, attemptID, key string, bound, recoveryMargin time.Duration) (store.BoardSegment, error)
	FinishBoardSegment(ctx context.Context, actor store.BoardActor, segmentID string, token board.Token, attemptID, outcome string) error
}

type segmentBeginRequest struct {
	ExpectedVersion   uint64 `json:"expected_version"`
	LeaseID           string `json:"lease_id"`
	Generation        uint64 `json:"generation"`
	AttemptID         string `json:"attempt_id"`
	Key               string `json:"key"`
	BoundMilliseconds int64  `json:"bound_milliseconds"`
	RecoveryMarginMS  int64  `json:"recovery_margin_ms"`
}

func (h *boardHTTP) segmentBegin(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.segment.begin")
	if !ok {
		return
	}
	st, ok := h.store.(durableBoardSegments)
	if !ok {
		problem(w, http.StatusServiceUnavailable, "unavailable", "durable board segments are not configured", true)
		return
	}
	var req segmentBeginRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.LeaseID) || !store.ValidID(req.AttemptID) || req.Generation == 0 || req.Key == "" ||
		req.BoundMilliseconds <= 0 || req.BoundMilliseconds > int64((24*time.Hour)/time.Millisecond) ||
		req.RecoveryMarginMS < 0 || req.RecoveryMarginMS > int64((24*time.Hour)/time.Millisecond) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid board segment request", false)
		return
	}
	segment, err := st.BeginBoardSegment(r.Context(), actor, req.ExpectedVersion,
		board.Token{BoardID: r.PathValue("board_id"), LeaseID: req.LeaseID, Generation: req.Generation},
		req.AttemptID, req.Key, time.Duration(req.BoundMilliseconds)*time.Millisecond,
		time.Duration(req.RecoveryMarginMS)*time.Millisecond)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, segment)
}

type segmentFinishRequest struct {
	LeaseID    string `json:"lease_id"`
	Generation uint64 `json:"generation"`
	AttemptID  string `json:"attempt_id"`
	Outcome    string `json:"outcome"`
}

func (h *boardHTTP) segmentFinish(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.segment.finish")
	if !ok {
		return
	}
	st, ok := h.store.(durableBoardSegments)
	if !ok {
		problem(w, http.StatusServiceUnavailable, "unavailable", "durable board segments are not configured", true)
		return
	}
	segmentID := r.PathValue("segment_id")
	if _, err := uuid.Parse(segmentID); err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid segment ID", false)
		return
	}
	var req segmentFinishRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.LeaseID) || !store.ValidID(req.AttemptID) || req.Generation == 0 ||
		(req.Outcome != "completed" && req.Outcome != "failed" && req.Outcome != "yielded") {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid board segment result", false)
		return
	}
	err := st.FinishBoardSegment(r.Context(), actor, segmentID,
		board.Token{BoardID: r.PathValue("board_id"), LeaseID: req.LeaseID, Generation: req.Generation}, req.AttemptID, req.Outcome)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"segment_id": segmentID, "outcome": req.Outcome})
}

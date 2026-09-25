// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Reading whether a board holder is still there, without being it.
//
// The beat endpoint answers the holder that just reported, so the only party
// that could see a holder's liveness was the holder itself. An operator
// deciding whether a quiet board has crashed, and the scheduler behind the
// queue it is holding up, both need the same answer and neither may beat: a
// beat is a claim about who is alive, and only the holder may make it.
//
// So this is a separate read rather than a field on the board status. Adding
// one to GET /v1/boards/{id} would be a wire change landed on both sides at
// once, because the status reply is the raw snapshot and every client decodes
// it with unknown fields disallowed. A board's recorded state and a judgement
// about its silence are also different answers with different lifetimes, and
// keeping them apart means a client that wants the snapshot alone is not made
// to carry a verdict it never asked for.
//
// It reports and decides nothing. A holder overdue here still holds the board
// until its own expiry, which is why reading this is safe for anyone the board
// already authorizes.
func (h *boardHTTP) liveness(w http.ResponseWriter, r *http.Request) {
	if _, ok := h.authorize(w, r, "board.liveness"); !ok {
		return
	}
	snapshot, err := h.store.GetBoard(r.Context(), r.PathValue("board_id"))
	if err != nil {
		writeBoardError(w, err)
		return
	}
	// One clock for the board and the judgement about its silence, and the
	// same interval the holder is told to report on, so an operator and a
	// holder never read two different deadlines for the same beat.
	reported, err := board.ObserveHolderLiveness(snapshot, time.Now().UTC(), h.heartbeatInterval)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Snapshot board.Snapshot     `json:"snapshot"`
		Liveness holderLivenessView `json:"liveness"`
	}{Snapshot: snapshot, Liveness: newHolderLivenessView(reported)})
}

package server

import (
	"context"
	"net/http"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type durableBoardHILHistory interface {
	BoardHILObservations(context.Context, store.BoardActor, catalog.HILTask) ([]hilspec.HistoricalObservation, error)
}

type hilHistoryRequest struct {
	TaskName string `json:"task_name"`
}

func (h *boardHTTP) hilObservationHistory(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.hil.history")
	if !ok {
		return
	}
	if h.catalog == nil || h.catalog.Digest() == "" {
		problem(w, http.StatusServiceUnavailable, "unavailable", "HIL catalog is not configured", true)
		return
	}
	var request hilHistoryRequest
	if !decodeBoardJSON(w, r, &request) {
		return
	}
	definition, found := h.catalog.Task(request.TaskName)
	if !found || definition.Scope != "hil" || definition.HIL == nil ||
		definition.HIL.BoardID != r.PathValue("board_id") {
		problem(w, http.StatusBadRequest, "invalid_argument", "task is not a HIL definition for this board", false)
		return
	}
	st, ok := h.store.(durableBoardHILHistory)
	if !ok {
		problem(w, http.StatusServiceUnavailable, "unavailable", "HIL history is not configured", true)
		return
	}
	observations, err := st.BoardHILObservations(r.Context(), actor, *definition.HIL)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"task_name": definition.Name, "observations": observations})
}

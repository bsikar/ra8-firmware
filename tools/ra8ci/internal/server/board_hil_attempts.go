package server

import (
	"context"
	"encoding/json"
	"net/http"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type durableBoardHILAttempts interface {
	StartBoardHILAttempt(context.Context, store.BoardActor, string, string, store.StartAttemptInput) (store.Attempt, error)
}

type startHILAttemptRequest struct {
	TaskID       string          `json:"task_id"`
	LeaseID      string          `json:"lease_id"`
	Host         string          `json:"host"`
	HostCores    int             `json:"host_cores"`
	HostRAMBytes int64           `json:"host_ram_bytes"`
	HostLoad     float64         `json:"host_load"`
	HostFacts    json.RawMessage `json:"host_facts"`
}

// startHILAttempt derives the lease holder and task actor inside the store. The
// board agent can report its own measurements, but cannot select the principal.
func (h *boardHTTP) startHILAttempt(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.hil.start")
	if !ok {
		return
	}
	st, ok := h.store.(durableBoardHILAttempts)
	if !ok {
		problem(w, http.StatusServiceUnavailable, "unavailable", "board HIL dispatch is not configured", true)
		return
	}
	var req startHILAttemptRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.TaskID) || !store.ValidID(req.LeaseID) || req.Host == "" ||
		req.HostCores < 1 || req.HostRAMBytes < 1 || req.HostLoad < 0 ||
		len(req.HostFacts) == 0 || !json.Valid(req.HostFacts) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid board HIL attempt request", false)
		return
	}
	attempt, err := st.StartBoardHILAttempt(r.Context(), actor, req.TaskID, req.LeaseID,
		store.StartAttemptInput{Engine: "board-agent", Host: req.Host,
			HostCores: req.HostCores, HostRAMBytes: req.HostRAMBytes,
			HostLoad: req.HostLoad, HostFacts: req.HostFacts})
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, attempt)
}

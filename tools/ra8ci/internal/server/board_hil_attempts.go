package server

import (
	"context"
	"encoding/json"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"net/http"
)

type durableBoardHILClaims interface {
	ClaimNextBoardHILAttempt(context.Context, store.BoardActor, string, store.StartAttemptInput, *catalog.Catalog, string) (*store.BoardHILAssignment, error)
}

type claimHILAttemptRequest struct {
	LeaseID      string          `json:"lease_id"`
	Host         string          `json:"host"`
	HostCores    int             `json:"host_cores"`
	HostRAMBytes int64           `json:"host_ram_bytes"`
	HostLoad     float64         `json:"host_load"`
	HostFacts    json.RawMessage `json:"host_facts"`
}

func (h *boardHTTP) claimNextHILAttempt(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.hil.claim")
	if !ok {
		return
	}
	st, ok := h.store.(durableBoardHILClaims)
	if !ok || h.catalog == nil || h.catalog.Digest() == "" || !protocol.ValidCommit(h.trustedCommit) {
		problem(w, http.StatusServiceUnavailable, "unavailable", "lease-aware HIL dispatch is not configured", true)
		return
	}
	var req claimHILAttemptRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.LeaseID) || req.Host == "" || req.HostCores < 1 ||
		req.HostRAMBytes < 1 || req.HostLoad < 0 || len(req.HostFacts) == 0 || !json.Valid(req.HostFacts) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid HIL claim request", false)
		return
	}
	assignment, err := st.ClaimNextBoardHILAttempt(r.Context(), actor, req.LeaseID,
		store.StartAttemptInput{Engine: "board-agent", Host: req.Host, HostCores: req.HostCores,
			HostRAMBytes: req.HostRAMBytes, HostLoad: req.HostLoad, HostFacts: req.HostFacts},
		h.catalog, h.trustedCommit)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	if assignment == nil {
		writeJSON(w, http.StatusOK, map[string]any{"assignment": nil})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"assignment": assignment})
}

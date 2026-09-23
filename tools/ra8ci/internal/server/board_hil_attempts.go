package server

import (
	"context"
	"encoding/json"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"net/http"
)

type durableBoardHILClaims interface {
	ClaimNextBoardHILAttempt(context.Context, store.BoardActor, string, store.StartAttemptInput, store.HILDefinitionCatalog, string) (*store.BoardHILAssignment, error)
}

type durableBoardHILFinisher interface {
	CompleteBoardHILAttempt(context.Context, store.BoardActor, store.BoardHILCompletion, store.HILDefinitionCatalog, string) error
}

type completeHILAttemptRequest struct {
	LeaseID          string          `json:"lease_id"`
	Generation       uint64          `json:"generation"`
	Result           string          `json:"result"`
	ChildExitCode    *int            `json:"child_exit_code,omitempty"`
	HitDeadline      bool            `json:"hit_deadline"`
	EvidenceComplete bool            `json:"evidence_complete"`
	Reason           string          `json:"reason,omitempty"`
	Steps            []store.HILStep `json:"steps"`
}

func (h *boardHTTP) completeHILAttempt(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.hil.complete")
	if !ok {
		return
	}
	st, ok := h.store.(durableBoardHILFinisher)
	if !ok || h.catalog == nil || h.catalog.Digest() == "" {
		problem(w, http.StatusServiceUnavailable, "unavailable", "lease-bound HIL completion is not configured", true)
		return
	}
	attemptID := r.PathValue("attempt_id")
	if !store.ValidID(attemptID) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid HIL attempt ID", false)
		return
	}
	var req completeHILAttemptRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.LeaseID) || req.Generation == 0 {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid HIL lease identity", false)
		return
	}
	err := st.CompleteBoardHILAttempt(r.Context(), actor, store.BoardHILCompletion{
		AttemptID: attemptID, LeaseID: req.LeaseID, Generation: req.Generation,
		Result: req.Result, ChildExitCode: req.ChildExitCode, HitDeadline: req.HitDeadline,
		EvidenceComplete: req.EvidenceComplete, Reason: req.Reason, Steps: req.Steps,
	}, h.catalog, h.trustedCommit)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"attempt_id": attemptID, "result": req.Result})
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

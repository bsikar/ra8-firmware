package server

import (
	"context"
	"crypto/sha256"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"mime"
	"net/http"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// BoardStore is the durable, cert-authorized board control surface. A test
// double may implement it, but production must pass the PostgreSQL store.
type BoardStore interface {
	AuthorizeBoardPeer(context.Context, *tls.ConnectionState, string, string) (store.BoardActor, error)
	GetBoard(context.Context, string) (board.Snapshot, error)
	ApplyBoardCommand(context.Context, store.BoardActor, board.Command, uint64, *store.NeutralSubmission, store.NeutralReceiptVerifier, time.Time) (board.Snapshot, []board.Event, error)
	IssueBoardNeutralChallenge(context.Context, store.BoardActor, uint64, string) (store.NeutralChallenge, error)
	AuditDenied(context.Context, string, string, string) error
}

type boardHTTP struct {
	store      BoardStore
	verifier   store.NeutralReceiptVerifier
	repository string
}

// RegisterBoardRoutes adds authenticated board endpoints to the server mux.
// A missing neutral verifier deliberately leaves release and recovery closed.
func RegisterBoardRoutes(mux *http.ServeMux, st BoardStore, verifier store.NeutralReceiptVerifier, repository string) error {
	if mux == nil || st == nil || strings.TrimSpace(repository) != repository || repository == "" {
		return store.ErrInvalid
	}
	h := &boardHTTP{store: st, verifier: verifier, repository: repository}
	mux.HandleFunc("GET /v1/boards/{board_id}", h.status)
	mux.HandleFunc("POST /v1/boards/{board_id}/take", h.take)
	mux.HandleFunc("POST /v1/boards/{board_id}/waiters/{waiter_id}/cancel", h.cancel)
	mux.HandleFunc("POST /v1/boards/{board_id}/checkpoint", h.checkpoint)
	mux.HandleFunc("POST /v1/boards/{board_id}/leases/{lease_id}/free", h.free)
	mux.HandleFunc("POST /v1/boards/{board_id}/leases/{lease_id}/extend", h.extend)
	mux.HandleFunc("POST /v1/boards/{board_id}/neutral-challenge", h.challenge)
	mux.HandleFunc("POST /v1/boards/{board_id}/agent/ack", h.agentAck)
	mux.HandleFunc("POST /v1/boards/{board_id}/agent/observe", h.agentObserve)
	mux.HandleFunc("POST /v1/boards/{board_id}/agent/unavailable", h.agentUnavailable)
	mux.HandleFunc("POST /v1/boards/{board_id}/recovery/start", h.recoveryStart)
	mux.HandleFunc("POST /v1/boards/{board_id}/quarantine", h.quarantine)
	return nil
}

func validHTTPBoardID(id string) bool {
	if id == "" || len(id) > 128 {
		return false
	}
	for _, c := range id {
		if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
			return false
		}
	}
	return true
}

func (h *boardHTTP) authorize(w http.ResponseWriter, r *http.Request, action string) (store.BoardActor, bool) {
	id := r.PathValue("board_id")
	if !validHTTPBoardID(id) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid board ID", false)
		return store.BoardActor{}, false
	}
	actor, err := h.store.AuthorizeBoardPeer(r.Context(), r.TLS, h.repository, id)
	if err == nil {
		return actor, true
	}
	peer := "unverified-peer"
	if r.TLS != nil && len(r.TLS.PeerCertificates) > 0 && r.TLS.PeerCertificates[0] != nil {
		sum := sha256.Sum256(r.TLS.PeerCertificates[0].Raw)
		peer = "certificate-sha256:" + hex.EncodeToString(sum[:])
	}
	if auditErr := h.store.AuditDenied(r.Context(), peer, action, id); auditErr != nil {
		problem(w, http.StatusServiceUnavailable, "unavailable", "authorization audit unavailable", true)
		return store.BoardActor{}, false
	}
	if errors.Is(err, store.ErrUnavailable) {
		problem(w, http.StatusServiceUnavailable, "unavailable", "authorization service unavailable", true)
	} else {
		problem(w, http.StatusNotFound, "denied", "board not found or access denied", false)
	}
	return store.BoardActor{}, false
}

func decodeBoardJSON(w http.ResponseWriter, r *http.Request, value any) bool {
	mediaType, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || mediaType != "application/json" {
		problem(w, http.StatusUnsupportedMediaType, "invalid_argument", "content type must be application/json", false)
		return false
	}
	r.Body = http.MaxBytesReader(w, r.Body, 128<<10)
	defer r.Body.Close()
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(value); err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid board request", false)
		return false
	}
	var trailing any
	if err := dec.Decode(&trailing); !errors.Is(err, io.EOF) {
		problem(w, http.StatusBadRequest, "invalid_argument", "trailing board request data", false)
		return false
	}
	return true
}

func writeBoardError(w http.ResponseWriter, err error) {
	var transition *board.Error
	if errors.As(err, &transition) {
		switch transition.Code {
		case board.InvalidArgument:
			problem(w, http.StatusBadRequest, string(transition.Code), transition.Error(), false)
		case board.Denied:
			problem(w, http.StatusForbidden, string(transition.Code), transition.Error(), false)
		default:
			problem(w, http.StatusConflict, string(transition.Code), transition.Error(), false)
		}
		return
	}
	if errors.Is(err, store.ErrDenied) {
		problem(w, http.StatusNotFound, "denied", "board not found or access denied", false)
		return
	}
	if errors.Is(err, store.ErrNotFound) {
		problem(w, http.StatusNotFound, "not_found", "board not found", false)
		return
	}
	writeStoreError(w, err)
}

func (h *boardHTTP) apply(w http.ResponseWriter, r *http.Request, actor store.BoardActor, command board.Command, expectedVersion uint64, neutral *store.NeutralSubmission) {
	snapshot, events, err := h.store.ApplyBoardCommand(r.Context(), actor, command, expectedVersion, neutral, h.verifier, time.Now().UTC())
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Snapshot board.Snapshot `json:"snapshot"`
		Events   []board.Event  `json:"events"`
	}{Snapshot: snapshot, Events: events})
}

func (h *boardHTTP) status(w http.ResponseWriter, r *http.Request) {
	if _, ok := h.authorize(w, r, "board.status"); !ok {
		return
	}
	snapshot, err := h.store.GetBoard(r.Context(), r.PathValue("board_id"))
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, snapshot)
}

type takeRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	RequestID       string `json:"request_id"`
	LeaseID         string `json:"lease_id"`
	Class           string `json:"class"`
	Why             string `json:"why"`
	DurationSeconds int64  `json:"duration_seconds"`
}

func takeClass(name string) board.Class {
	switch name {
	case "human":
		return board.ClassHuman
	case "ci":
		return board.ClassCI
	case "agent":
		return board.ClassAI
	default:
		return 0
	}
}

func (h *boardHTTP) take(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.take")
	if !ok {
		return
	}
	var req takeRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.RequestID) || !store.ValidID(req.LeaseID) || req.RequestID == req.LeaseID ||
		takeClass(req.Class) == 0 || req.DurationSeconds <= 0 || req.DurationSeconds > 8*60*60 ||
		len(req.Why) == 0 || len(req.Why) > 500 || strings.TrimSpace(req.Why) != req.Why {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid board take request", false)
		return
	}
	h.apply(w, r, actor, board.Enqueue{Waiter: board.Waiter{
		ID: req.RequestID, LeaseID: req.LeaseID, Holder: actor.ID(),
		Class: takeClass(req.Class), Reason: req.Why, Duration: time.Duration(req.DurationSeconds) * time.Second,
	}}, req.ExpectedVersion, nil)
}

type versionRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
}

func (h *boardHTTP) cancel(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.cancel")
	if !ok {
		return
	}
	if !store.ValidID(r.PathValue("waiter_id")) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid waiter ID", false)
		return
	}
	var req versionRequest
	if decodeBoardJSON(w, r, &req) {
		h.apply(w, r, actor, board.CancelWaiter{WaiterID: r.PathValue("waiter_id")}, req.ExpectedVersion, nil)
	}
}

type leaseCommandRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	LeaseID         string `json:"lease_id"`
	Generation      uint64 `json:"generation"`
}

func (h *boardHTTP) checkpoint(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.checkpoint")
	if !ok {
		return
	}
	var req leaseCommandRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.LeaseID) || req.Generation == 0 {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid lease token", false)
		return
	}
	h.apply(w, r, actor, board.BeginDrain{LeaseID: req.LeaseID, Generation: req.Generation}, req.ExpectedVersion, nil)
}

type freeRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	Generation      uint64 `json:"generation"`
	ChallengeID     string `json:"challenge_id"`
	Receipt         []byte `json:"receipt"`
}

func (h *boardHTTP) free(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.free")
	if !ok {
		return
	}
	if h.verifier == nil {
		problem(w, http.StatusServiceUnavailable, "unavailable", "neutral receipt verifier is not configured", true)
		return
	}
	leaseID := r.PathValue("lease_id")
	var req freeRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(leaseID) || req.Generation == 0 || !store.ValidID(req.ChallengeID) || len(req.Receipt) == 0 || len(req.Receipt) > 65536 {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid neutral release submission", false)
		return
	}
	h.apply(w, r, actor, board.Release{LeaseID: leaseID, Generation: req.Generation}, req.ExpectedVersion,
		&store.NeutralSubmission{ChallengeID: req.ChallengeID, Receipt: req.Receipt})
}

type extendRequest struct {
	ExpectedVersion uint64    `json:"expected_version"`
	Generation      uint64    `json:"generation"`
	NewExpiry       time.Time `json:"new_expiry"`
	Why             string    `json:"why"`
}

func (h *boardHTTP) extend(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.extend")
	if !ok {
		return
	}
	leaseID := r.PathValue("lease_id")
	var req extendRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(leaseID) || req.Generation == 0 || req.NewExpiry.IsZero() || req.Why == "" || len(req.Why) > 500 {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid extension request", false)
		return
	}
	h.apply(w, r, actor, board.Extend{LeaseID: leaseID, Generation: req.Generation,
		NewExpiry: req.NewExpiry, Reason: req.Why}, req.ExpectedVersion, nil)
}

type challengeRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	Purpose         string `json:"purpose"`
}

func (h *boardHTTP) challenge(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.neutral_challenge")
	if !ok {
		return
	}
	if h.verifier == nil {
		problem(w, http.StatusServiceUnavailable, "unavailable", "neutral receipt verifier is not configured", true)
		return
	}
	var req challengeRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if req.Purpose != "release" && req.Purpose != "recovery" {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid neutral challenge purpose", false)
		return
	}
	challenge, err := h.store.IssueBoardNeutralChallenge(r.Context(), actor, req.ExpectedVersion, req.Purpose)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, challenge)
}

type ackRequest struct {
	ExpectedVersion     uint64 `json:"expected_version"`
	LeaseID             string `json:"lease_id"`
	Generation          uint64 `json:"generation"`
	InstalledGeneration uint64 `json:"installed_generation"`
}

func (h *boardHTTP) agentAck(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.agent.ack")
	if !ok {
		return
	}
	var req ackRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.LeaseID) || req.Generation == 0 || req.InstalledGeneration == 0 {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid installed generation", false)
		return
	}
	h.apply(w, r, actor, board.AcknowledgeGrant{LeaseID: req.LeaseID,
		Generation: req.Generation, InstalledGeneration: req.InstalledGeneration}, req.ExpectedVersion, nil)
}

type observeRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	HighWater       uint64 `json:"high_water"`
}

func (h *boardHTTP) agentObserve(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.agent.observe")
	if !ok {
		return
	}
	var req observeRequest
	if decodeBoardJSON(w, r, &req) {
		h.apply(w, r, actor, board.ObserveAgentGeneration{HighWater: req.HighWater}, req.ExpectedVersion, nil)
	}
}

type reasonRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	Why             string `json:"why"`
}

func validReason(reason string) bool {
	return reason != "" && len(reason) <= 500 && strings.TrimSpace(reason) == reason
}

func (h *boardHTTP) agentUnavailable(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.agent.unavailable")
	if !ok {
		return
	}
	var req reasonRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !validReason(req.Why) {
		problem(w, http.StatusBadRequest, "invalid_argument", "reason is required", false)
		return
	}
	h.apply(w, r, actor, board.AgentUnavailable{Reason: req.Why}, req.ExpectedVersion, nil)
}

type recoveryStartRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	PlanID          string `json:"plan_id"`
	Why             string `json:"why"`
}

func (h *boardHTTP) recoveryStart(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.recovery.start")
	if !ok {
		return
	}
	var req recoveryStartRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.PlanID) || !validReason(req.Why) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid recovery plan", false)
		return
	}
	h.apply(w, r, actor, board.BeginRecovery{PlanID: req.PlanID, Reason: req.Why}, req.ExpectedVersion, nil)
}

func (h *boardHTTP) quarantine(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.quarantine")
	if !ok {
		return
	}
	var req reasonRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !validReason(req.Why) {
		problem(w, http.StatusBadRequest, "invalid_argument", "reason is required", false)
		return
	}
	h.apply(w, r, actor, board.Quarantine{Reason: req.Why}, req.ExpectedVersion, nil)
}

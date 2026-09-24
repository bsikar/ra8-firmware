// Package server exposes authenticated, versioned control-plane admission and status.
package server

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/migrations"
)

// Authorizer returns a server-derived principal for a repository action.
type Authorizer interface {
	Authorize(*http.Request, string, string) (string, error)
}

type runLogReader interface {
	LookupRunRepository(context.Context, string) (string, error)
	AttemptLogs(context.Context, string, string, int64, int) (store.LogPage, error)
}

// MTLSAuthorizer requires a verified TLS client certificate and a current
// PostgreSQL grant. A body field can never claim human or service identity.
type MTLSAuthorizer struct{ Store *store.Store }

func (a MTLSAuthorizer) Authorize(r *http.Request, repository, permission string) (string, error) {
	if a.Store == nil || r.TLS == nil || len(r.TLS.VerifiedChains) == 0 || len(r.TLS.VerifiedChains[0]) == 0 || len(r.TLS.PeerCertificates) == 0 || len(r.TLS.PeerCertificates[0].Raw) == 0 {
		return "", store.ErrDenied
	}
	leaf := r.TLS.PeerCertificates[0]
	if r.TLS.VerifiedChains[0][0] == nil || !leaf.Equal(r.TLS.VerifiedChains[0][0]) ||
		time.Now().Before(leaf.NotBefore) || !time.Now().Before(leaf.NotAfter) {
		return "", store.ErrDenied
	}
	return a.Store.AuthorizeCertificate(r.Context(), leaf.Raw, repository, permission)
}

type Server struct {
	store              *store.Store
	logReader          runLogReader
	catalog            *catalog.Catalog
	auth               Authorizer
	mux                *http.ServeMux
	trustedAgentCommit string
	readinessChecks    []func(context.Context) error
}

// New installs only implemented endpoints with the mTLS authorizer. An absent
// catalog/store is a configuration error, never an unauthenticated fallback.
func New(st *store.Store, cat *catalog.Catalog) (*Server, error) {
	return NewWithOptions(st, cat, nil, "")
}

// NewWithBoardVerifier enables lease transitions only when a reviewed,
// independently authenticated neutral-receipt verifier is configured.
func NewWithBoardVerifier(st *store.Store, cat *catalog.Catalog, verifier store.NeutralReceiptVerifier) (*Server, error) {
	return NewWithOptions(st, cat, verifier, "")
}

// NewWithOptions requires an exact reviewed commit before any remote script
// task may be claimed. Empty trust keeps the agent dispatch endpoint closed.
func NewWithOptions(st *store.Store, cat *catalog.Catalog, verifier store.NeutralReceiptVerifier, trustedAgentCommit string, readinessChecks ...func(context.Context) error) (*Server, error) {
	if st == nil || cat == nil || cat.Digest() == "" {
		return nil, fmt.Errorf("%w: missing server dependency", store.ErrInvalid)
	}
	if trustedAgentCommit != "" && !protocol.ValidCommit(trustedAgentCommit) {
		return nil, fmt.Errorf("%w: invalid trusted agent commit", store.ErrInvalid)
	}
	for _, check := range readinessChecks {
		if check == nil {
			return nil, fmt.Errorf("%w: nil readiness check", store.ErrInvalid)
		}
	}
	s := &Server{store: st, logReader: st, catalog: cat, auth: MTLSAuthorizer{Store: st},
		mux: http.NewServeMux(), trustedAgentCommit: trustedAgentCommit,
		readinessChecks: append([]func(context.Context) error(nil), readinessChecks...)}
	s.mux.HandleFunc("GET /health/live", s.live)
	s.mux.HandleFunc("GET /health/ready", s.ready)
	s.mux.HandleFunc("POST /v1/runs", s.createRun)
	s.mux.HandleFunc("GET /v1/runs/{id}", s.getRun)
	s.mux.HandleFunc("POST /v1/runs/{id}/cancel", s.cancelRun)
	s.mux.HandleFunc("GET /v1/runs/{id}/logs", s.getRunLogs)
	s.mux.HandleFunc("GET /v1/runs/{id}/events", s.getRunEvents)
	s.mux.HandleFunc("POST /v1/local-runs/sync", s.ingestOffline)
	s.mux.HandleFunc("GET /v1/reports/slow", s.slowReport)
	s.mux.HandleFunc("POST /v1/agents/me/claim", s.agentClaim)
	s.mux.HandleFunc("/v1/terraform/runner-states/{reservation_id}", s.terraformState)
	s.mux.HandleFunc("POST /v1/assignments/{assignment_id}/ack", s.agentAck)
	s.mux.HandleFunc("POST /v1/attempts/{attempt_id}/logs", s.agentLog)
	s.mux.HandleFunc("POST /v1/attempts/{attempt_id}/result", s.agentResult)
	s.mux.HandleFunc("POST /v1/attempts/{attempt_id}/artifacts/chunk", s.agentArtifactChunk)
	s.mux.HandleFunc("POST /v1/attempts/{attempt_id}/artifacts/manifest", s.agentArtifactManifest)
	s.mux.HandleFunc("POST /v1/agents/me/heartbeat", s.agentHeartbeat)
	if err := RegisterBoardRoutes(s.mux, st, verifier, "bsikar/ra8-firmware", BoardHILPolicy{Catalog: cat, TrustedCommit: trustedAgentCommit}); err != nil {
		return nil, fmt.Errorf("register board routes: %w", err)
	}
	return s, nil
}

func (s *Server) Handler() http.Handler { return s.mux }

func (s *Server) live(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"status": "alive"})
}

func (s *Server) ready(w http.ResponseWriter, r *http.Request) {
	if err := s.store.Health(r.Context()); err != nil {
		problem(w, http.StatusServiceUnavailable, "unavailable", "database or audit is not writable", true)
		return
	}
	if err := s.checkReadiness(r.Context()); err != nil {
		problem(w, http.StatusServiceUnavailable, "unavailable", "a configured readiness dependency is unavailable", true)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"status": "ready", "schema": migrations.CurrentVersion()})
}

func (s *Server) checkReadiness(ctx context.Context) error {
	for _, check := range s.readinessChecks {
		if err := check(ctx); err != nil {
			return err
		}
	}
	return nil
}

type createRequest struct {
	Trigger       string        `json:"trigger"`
	Source        sourceRequest `json:"source"`
	CatalogDigest string        `json:"catalog_digest"`
	Tasks         []taskRequest `json:"tasks"`
	ParentRunID   string        `json:"parent_run_id"`
}

type sourceRequest struct {
	Repository     string `json:"repo"`
	Branch         string `json:"branch"`
	CommitSHA      string `json:"commit"`
	SnapshotSHA256 string `json:"snapshot_sha256"`
}

// taskRequest asks for one task within a run.
//
// Values carries the task's arguments by NAME. Args is the older field and
// must stay empty: the plane derives argv itself, from these names and the
// reviewed schema it already holds, under the catalog digest both sides have
// agreed on. A submitter therefore cannot state an argv element, which means
// it cannot state one that no binding of a reviewed argument could produce.
type taskRequest struct {
	Key           string            `json:"key"`
	Name          string            `json:"name"`
	Args          []string          `json:"args"`
	Values        map[string]string `json:"values"`
	DependsOnKeys []string          `json:"depends_on_keys"`
}

// persistedTaskArguments writes what a task will run with: the argv the plane
// bound, and the named values it bound them from.
//
// Both are stored because they answer different questions later. argv is what
// the agent is handed; the values are the record of what was actually asked
// for, and they are what a re-validating reader re-binds to check that argv
// still matches the catalog it holds. The values key is omitted when there
// are none, so a task with no arguments keeps the exact stored shape every
// row written before this had.
func persistedTaskArguments(definition catalog.Task, values map[string]string) (json.RawMessage, error) {
	argv, err := definition.BindArguments(values)
	if err != nil {
		return nil, err
	}
	if argv == nil {
		argv = []string{}
	}
	argumentData := map[string]any{"argv": argv}
	if definition.HIL != nil {
		argumentData["hil"] = definition.HIL
	}
	if len(values) != 0 {
		argumentData["values"] = values
	}
	return json.Marshal(argumentData)
}

func (s *Server) createRun(w http.ResponseWriter, r *http.Request) {
	if !strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
		problem(w, http.StatusUnsupportedMediaType, "invalid_argument", "content type must be application/json", false)
		return
	}
	key := r.Header.Get("Idempotency-Key")
	if len(key) == 0 || len(key) > 256 {
		problem(w, http.StatusBadRequest, "invalid_argument", "Idempotency-Key is required (1..256 bytes)", false)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	defer r.Body.Close()
	raw, err := io.ReadAll(r.Body)
	if err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "request body exceeds limit or is unreadable", false)
		return
	}
	var req createRequest
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid run request", false)
		return
	}
	var trailing any
	if err := dec.Decode(&trailing); !errors.Is(err, io.EOF) {
		problem(w, http.StatusBadRequest, "invalid_argument", "trailing JSON data", false)
		return
	}
	principal, err := s.auth.Authorize(r, req.Source.Repository, "submit")
	if err != nil {
		s.deny(w, r, "run.create", req.Source.Repository, err)
		return
	}
	if req.CatalogDigest != s.catalog.Digest() {
		problem(w, http.StatusConflict, "protocol_mismatch", "task catalog digest differs from server", false)
		return
	}
	input := store.CreateRunInput{
		Trigger: req.Trigger, ActorID: principal, Repository: req.Source.Repository,
		Branch: req.Source.Branch, CommitSHA: req.Source.CommitSHA,
		SnapshotSHA256: req.Source.SnapshotSHA256, CatalogSHA256: req.CatalogDigest,
		ParentRunID: req.ParentRunID, IdempotencyKey: key,
	}
	sum := sha256.Sum256(raw)
	input.RequestSHA256 = hex.EncodeToString(sum[:])
	for _, requested := range req.Tasks {
		definition, found := s.catalog.Task(requested.Name)
		if !found || definition.ValidateArguments(requested.Args) != nil {
			problem(w, http.StatusBadRequest, "invalid_argument", "unknown task or invalid task arguments", false)
			return
		}
		arguments, err := persistedTaskArguments(definition, requested.Values)
		if err != nil {
			problem(w, http.StatusBadRequest, "invalid_argument", "invalid task arguments", false)
			return
		}
		input.Tasks = append(input.Tasks, store.TaskInput{
			Key: requested.Key, Name: requested.Name, Arguments: arguments,
			DependsOnKeys: requested.DependsOnKeys, Tier: definition.Tier,
			Scope: definition.Scope, HostClass: definition.Scope,
			DeadlineSeconds: definition.DeadlineSeconds,
		})
	}
	run, err := s.store.CreateRun(r.Context(), input)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	w.Header().Set("Location", "/v1/runs/"+run.ID)
	writeJSON(w, http.StatusCreated, map[string]any{"id": run.ID, "state": run.State})
}

func (s *Server) getRun(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !store.ValidID(id) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid run ID", false)
		return
	}
	repository, err := s.store.LookupRunRepository(r.Context(), id)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	if _, err := s.auth.Authorize(r, repository, "read"); err != nil {
		s.deny(w, r, "run.read", id, err)
		return
	}
	run, err := s.store.GetRun(r.Context(), id)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, run)
}

func (s *Server) deny(w http.ResponseWriter, r *http.Request, action, target string, authErr error) {
	actor := "unverified-peer"
	if r.TLS != nil && len(r.TLS.PeerCertificates) > 0 {
		sum := sha256.Sum256(r.TLS.PeerCertificates[0].Raw)
		actor = "certificate-sha256:" + hex.EncodeToString(sum[:])
	}
	if err := s.store.AuditDenied(r.Context(), actor, action, target); err != nil {
		problem(w, http.StatusServiceUnavailable, "unavailable", "authorization audit unavailable", true)
		return
	}
	if errors.Is(authErr, store.ErrUnavailable) {
		problem(w, http.StatusServiceUnavailable, "unavailable", "authorization service unavailable", true)
		return
	}
	problem(w, http.StatusNotFound, "denied", "run not found or access denied", false)
}

func writeStoreError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, store.ErrInvalid):
		problem(w, http.StatusBadRequest, "invalid_argument", err.Error(), false)
	case errors.Is(err, store.ErrConflict):
		problem(w, http.StatusConflict, "conflict", err.Error(), false)
	case errors.Is(err, store.ErrNotFound):
		problem(w, http.StatusNotFound, "not_found", "run not found", false)
	default:
		problem(w, http.StatusServiceUnavailable, "unavailable", "database operation unavailable", true)
	}
}

func problem(w http.ResponseWriter, status int, code, detail string, retryable bool) {
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]any{
		"type": "about:blank", "status": status, "code": code,
		"detail": detail, "retryable": retryable,
	})
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

package server

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func certificateFingerprint(der []byte) string {
	sum := sha256.Sum256(der)
	return hex.EncodeToString(sum[:])
}

// verifiedAgentCertificate derives the transport identity from a TLS chain
// verified by the listener. JSON fields never become an actor identity.
func verifiedAgentCertificate(r *http.Request) ([]byte, error) {
	if r.TLS == nil || len(r.TLS.VerifiedChains) == 0 ||
		len(r.TLS.VerifiedChains[0]) == 0 || len(r.TLS.PeerCertificates) == 0 ||
		r.TLS.VerifiedChains[0][0] == nil || r.TLS.PeerCertificates[0] == nil ||
		!r.TLS.PeerCertificates[0].Equal(r.TLS.VerifiedChains[0][0]) ||
		len(r.TLS.PeerCertificates[0].Raw) == 0 ||
		time.Now().Before(r.TLS.PeerCertificates[0].NotBefore) ||
		!time.Now().Before(r.TLS.PeerCertificates[0].NotAfter) {
		return nil, store.ErrDenied
	}
	return r.TLS.PeerCertificates[0].Raw, nil
}

func decodeAgentRequest(w http.ResponseWriter, r *http.Request, value any) bool {
	if !strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
		problem(w, http.StatusUnsupportedMediaType, "invalid_argument", "content type must be application/json", false)
		return false
	}
	r.Body = http.MaxBytesReader(w, r.Body, protocol.MaxJSONBytes)
	defer r.Body.Close()
	if err := protocol.DecodeStrict(r.Body, value); err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid agent request", false)
		return false
	}
	return true
}

func (s *Server) agentFailure(w http.ResponseWriter, r *http.Request, action, target string, err error) {
	if errors.Is(err, store.ErrDenied) {
		s.deny(w, r, action, target, err)
		return
	}
	if errors.Is(err, store.ErrConflict) {
		auditor := s.denialAudit()
		if auditor == nil {
			problem(w, http.StatusServiceUnavailable, "unavailable", "conflict audit unavailable", true)
			return
		}
		if auditErr := auditor.AuditDenied(r.Context(), certificateActor(r), action+".conflict", target); auditErr != nil {
			problem(w, http.StatusServiceUnavailable, "unavailable", "conflict audit unavailable", true)
			return
		}
	}
	writeStoreError(w, err)
}

func accepted(version, fence int64) protocol.AcceptResponse {
	return protocol.AcceptResponse{SchemaVersion: protocol.Version,
		AssignmentVersion: version, FencingToken: fence, Accepted: true}
}

func (s *Server) agentClaim(w http.ResponseWriter, r *http.Request) {
	if s.trustedAgentCommit == "" {
		problem(w, http.StatusServiceUnavailable, "unavailable", "trusted agent commit is not configured", true)
		return
	}
	cert, err := verifiedAgentCertificate(r)
	if err != nil {
		s.agentFailure(w, r, "agent.claim", "agent", err)
		return
	}
	var request protocol.ClaimRequest
	if !decodeAgentRequest(w, r, &request) {
		return
	}
	if err := request.Validate(); err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid agent claim", false)
		return
	}
	assignment, err := s.store.ClaimAgentTask(r.Context(), cert, request.HostFacts, s.catalog, s.trustedAgentCommit)
	if err != nil {
		s.agentFailure(w, r, "agent.claim", "agent", err)
		return
	}
	if assignment == nil {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	writeJSON(w, http.StatusOK, assignment)
}

func (s *Server) agentAck(w http.ResponseWriter, r *http.Request) {
	cert, err := verifiedAgentCertificate(r)
	if err != nil {
		s.agentFailure(w, r, "agent.ack", r.PathValue("assignment_id"), err)
		return
	}
	var ack protocol.Ack
	if !decodeAgentRequest(w, r, &ack) {
		return
	}
	if ack.AssignmentID != r.PathValue("assignment_id") || ack.Validate() != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "assignment identity mismatch", false)
		return
	}
	if err := s.store.AcknowledgeAgentAssignment(r.Context(), cert, ack); err != nil {
		s.agentFailure(w, r, "agent.ack", ack.AssignmentID, err)
		return
	}
	writeJSON(w, http.StatusOK, accepted(ack.AssignmentVersion, ack.FencingToken))
}

func (s *Server) agentLog(w http.ResponseWriter, r *http.Request) {
	cert, err := verifiedAgentCertificate(r)
	if err != nil {
		s.agentFailure(w, r, "agent.log", r.PathValue("attempt_id"), err)
		return
	}
	var chunk protocol.LogChunk
	if !decodeAgentRequest(w, r, &chunk) {
		return
	}
	if chunk.AttemptID != r.PathValue("attempt_id") || chunk.Validate() != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "attempt identity or log digest mismatch", false)
		return
	}
	if err := s.store.SaveAgentLog(r.Context(), cert, chunk); err != nil {
		s.agentFailure(w, r, "agent.log", chunk.AttemptID, err)
		return
	}
	writeJSON(w, http.StatusOK, accepted(chunk.AssignmentVersion, chunk.FencingToken))
}

func (s *Server) agentResult(w http.ResponseWriter, r *http.Request) {
	cert, err := verifiedAgentCertificate(r)
	if err != nil {
		s.agentFailure(w, r, "agent.result", r.PathValue("attempt_id"), err)
		return
	}
	var receipt protocol.TerminalReceipt
	if !decodeAgentRequest(w, r, &receipt) {
		return
	}
	if receipt.AttemptID != r.PathValue("attempt_id") || receipt.Validate() != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid terminal receipt", false)
		return
	}
	if err := s.store.CompleteAgentAttempt(r.Context(), cert, receipt, s.catalog); err != nil {
		s.agentFailure(w, r, "agent.result", receipt.AttemptID, err)
		return
	}
	writeJSON(w, http.StatusOK, accepted(receipt.AssignmentVersion, receipt.FencingToken))
}

func (s *Server) agentHeartbeat(w http.ResponseWriter, r *http.Request) {
	cert, err := verifiedAgentCertificate(r)
	if err != nil {
		s.agentFailure(w, r, "agent.heartbeat", "agent", err)
		return
	}
	var heartbeat protocol.Heartbeat
	if !decodeAgentRequest(w, r, &heartbeat) {
		return
	}
	if err := heartbeat.Validate(); err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid agent heartbeat", false)
		return
	}
	response, err := s.store.HeartbeatAgentAttempt(r.Context(), cert, heartbeat)
	if err != nil {
		s.agentFailure(w, r, "agent.heartbeat", heartbeat.AttemptID, err)
		return
	}
	writeJSON(w, http.StatusOK, response)
}

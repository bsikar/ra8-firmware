// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// agentArtifactChunk takes one ordered slice of a step's declared output.
// The path in the body is checked by protocol validation, never trusted as a
// filesystem path here: nothing on this side opens a file.
func (s *Server) agentArtifactChunk(w http.ResponseWriter, r *http.Request) {
	cert, err := verifiedAgentCertificate(r)
	if err != nil {
		s.agentFailure(w, r, "agent.artifact", r.PathValue("attempt_id"), err)
		return
	}
	var chunk protocol.ArtifactChunk
	if !decodeAgentRequest(w, r, &chunk) {
		return
	}
	if chunk.AttemptID != r.PathValue("attempt_id") || chunk.Validate() != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "attempt identity or artifact digest mismatch", false)
		return
	}
	if _, err := s.store.SaveAgentArtifactChunk(r.Context(), cert, chunk); err != nil {
		s.agentFailure(w, r, "agent.artifact", chunk.AttemptID, err)
		return
	}
	writeJSON(w, http.StatusOK, accepted(chunk.AssignmentVersion, chunk.FencingToken))
}

// agentArtifactManifest closes one artifact. A duplicate manifest is the same
// 200 as the first, so a retrying agent never has to decide whether its
// previous close landed.
func (s *Server) agentArtifactManifest(w http.ResponseWriter, r *http.Request) {
	cert, err := verifiedAgentCertificate(r)
	if err != nil {
		s.agentFailure(w, r, "agent.artifact.manifest", r.PathValue("attempt_id"), err)
		return
	}
	var manifest protocol.ArtifactManifest
	if !decodeAgentRequest(w, r, &manifest) {
		return
	}
	if manifest.AttemptID != r.PathValue("attempt_id") || manifest.Validate() != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "attempt identity or artifact manifest mismatch", false)
		return
	}
	if _, err := s.store.CloseAgentArtifact(r.Context(), cert, manifest); err != nil {
		s.agentFailure(w, r, "agent.artifact.manifest", manifest.AttemptID, err)
		return
	}
	writeJSON(w, http.StatusOK, accepted(manifest.AssignmentVersion, manifest.FencingToken))
}

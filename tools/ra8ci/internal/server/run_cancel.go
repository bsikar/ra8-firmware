// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func (s *Server) cancelRun(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !store.ValidID(id) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid run ID", false)
		return
	}
	if r.ContentLength != 0 {
		problem(w, http.StatusBadRequest, "invalid_argument", "run cancellation does not accept a request body", false)
		return
	}
	repository, err := s.store.LookupRunRepository(r.Context(), id)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	principal, err := s.auth.Authorize(r, repository, "submit")
	if err != nil {
		s.deny(w, r, "run.cancel", id, err)
		return
	}
	run, err := s.store.RequestRunCancellation(r.Context(), id, principal)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, run)
}

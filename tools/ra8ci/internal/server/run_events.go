// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"
	"net/url"
	"strconv"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func parseEventQuery(query url.Values) (int64, int, bool) {
	for key := range query {
		if key != "after" && key != "limit" {
			return 0, 0, false
		}
	}
	after := int64(0)
	if values, found := query["after"]; found {
		if len(values) != 1 {
			return 0, 0, false
		}
		parsed, err := strconv.ParseInt(values[0], 10, 64)
		if err != nil || parsed < 0 {
			return 0, 0, false
		}
		after = parsed
	}
	limit := store.MaxEventPageSize
	if values, found := query["limit"]; found {
		if len(values) != 1 {
			return 0, 0, false
		}
		parsed, err := strconv.Atoi(values[0])
		if err != nil || parsed < 1 || parsed > store.MaxEventPageSize {
			return 0, 0, false
		}
		limit = parsed
	}
	return after, limit, true
}

func (s *Server) getRunEvents(w http.ResponseWriter, r *http.Request) {
	runID := r.PathValue("id")
	if !store.ValidID(runID) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid run ID", false)
		return
	}
	after, limit, valid := parseEventQuery(r.URL.Query())
	if !valid {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid run event query", false)
		return
	}
	repository, err := s.store.LookupRunRepository(r.Context(), runID)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	if _, err := s.auth.Authorize(r, repository, "read"); err != nil {
		s.deny(w, r, "run.events", runID, err)
		return
	}
	page, err := s.store.RunEvents(r.Context(), runID, after, limit)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	if page.RunID != runID || len(page.Events) > limit || page.NextAfter < after ||
		page.NextAfter-after > int64(limit) || (page.HasMore && len(page.Events) != limit) {
		problem(w, http.StatusServiceUnavailable, "unavailable", "stored run event page is inconsistent", true)
		return
	}
	writeJSON(w, http.StatusOK, page)
}

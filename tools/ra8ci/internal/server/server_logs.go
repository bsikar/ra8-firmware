// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"
	"net/url"
	"strconv"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func parseLogQuery(query url.Values) (string, int64, int, bool) {
	for key := range query {
		if key != "attempt_id" && key != "after" && key != "limit" {
			return "", 0, 0, false
		}
	}
	attemptID := query.Get("attempt_id")
	if !store.ValidID(attemptID) || len(query["attempt_id"]) != 1 {
		return "", 0, 0, false
	}
	after := int64(0)
	if values, found := query["after"]; found {
		if len(values) != 1 {
			return "", 0, 0, false
		}
		parsed, err := strconv.ParseInt(values[0], 10, 64)
		if err != nil || parsed < 0 {
			return "", 0, 0, false
		}
		after = parsed
	}
	limit := store.MaxLogPageSize
	if values, found := query["limit"]; found {
		if len(values) != 1 {
			return "", 0, 0, false
		}
		parsed, err := strconv.Atoi(values[0])
		if err != nil || parsed < 1 || parsed > store.MaxLogPageSize {
			return "", 0, 0, false
		}
		limit = parsed
	}
	return attemptID, after, limit, true
}

func (s *Server) getRunLogs(w http.ResponseWriter, r *http.Request) {
	runID := r.PathValue("id")
	if !store.ValidID(runID) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid run ID", false)
		return
	}
	attemptID, after, limit, valid := parseLogQuery(r.URL.Query())
	if !valid {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid log page query", false)
		return
	}
	if s.logReader == nil {
		problem(w, http.StatusServiceUnavailable, "unavailable", "run log reader is not configured", true)
		return
	}
	repository, err := s.logReader.LookupRunRepository(r.Context(), runID)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	if _, err := s.auth.Authorize(r, repository, "read"); err != nil {
		s.deny(w, r, "run.logs", runID, err)
		return
	}
	page, err := s.logReader.AttemptLogs(r.Context(), runID, attemptID, after, limit)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	pageInvalid := page.AttemptID != attemptID || len(page.Chunks) > limit ||
		page.NextAfter < after || page.NextAfter-after > int64(limit)
	if len(page.Chunks) == 0 {
		pageInvalid = pageInvalid || page.NextAfter != after || page.HasMore
	} else {
		pageInvalid = pageInvalid || page.Chunks[len(page.Chunks)-1].Sequence != page.NextAfter ||
			(page.HasMore && len(page.Chunks) != limit)
	}
	if pageInvalid {
		problem(w, http.StatusServiceUnavailable, "unavailable", "stored log page is inconsistent", true)
		return
	}
	writeJSON(w, http.StatusOK, page)
}

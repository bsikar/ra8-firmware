package server

import (
	"net/http"
	"strconv"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type slowReportResponse struct {
	Repository    string           `json:"repository"`
	WindowSeconds int64            `json:"window_seconds"`
	Tasks         []store.SlowTask `json:"tasks"`
}

// slowReport keeps PostgreSQL behind the control plane and authorizes the
// requested repository from the peer certificate rather than caller claims.
func (s *Server) slowReport(w http.ResponseWriter, r *http.Request) {
	query := r.URL.Query()
	if len(query) != 3 || len(query["repository"]) != 1 ||
		len(query["window_seconds"]) != 1 || len(query["limit"]) != 1 {
		problem(w, http.StatusBadRequest, "invalid_argument", "repository, window_seconds, and limit are required", false)
		return
	}
	repository := query.Get("repository")
	seconds, secondsErr := strconv.ParseInt(query.Get("window_seconds"), 10, 64)
	limit, limitErr := strconv.Atoi(query.Get("limit"))
	if repository == "" || len(repository) > 512 || secondsErr != nil ||
		seconds < 1 || seconds > int64((365*24*time.Hour)/time.Second) ||
		limitErr != nil || limit < 1 || limit > 500 {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid slow report window, repository, or limit", false)
		return
	}
	if _, err := s.auth.Authorize(r, repository, "read"); err != nil {
		s.deny(w, r, "report.slow", repository, err)
		return
	}
	rows, err := s.store.SlowTasks(r.Context(), repository, time.Now().Add(-time.Duration(seconds)*time.Second), limit)
	if err != nil {
		writeStoreError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, slowReportResponse{Repository: repository, WindowSeconds: seconds, Tasks: rows})
}

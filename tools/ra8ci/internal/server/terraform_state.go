package server

import (
	"errors"
	"io"
	"net/http"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const maxTerraformStateBody = 16 << 20

func (s *Server) terraformState(w http.ResponseWriter, r *http.Request) {
	reservationID := r.PathValue("reservation_id")
	if !store.ValidID(reservationID) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid runner reservation ID", false)
		return
	}
	repository, err := s.store.LookupRunnerVMRepository(r.Context(), reservationID)
	if err != nil {
		writeTerraformStateError(w, err)
		return
	}
	actor, err := s.auth.Authorize(r, repository, "terraform_state")
	if err != nil {
		s.deny(w, r, terraformStateAction(r.Method), reservationID, err)
		return
	}

	switch r.Method {
	case http.MethodGet:
		body, found, err := s.store.ReadRunnerVMTerraformState(r.Context(), reservationID)
		if err != nil {
			writeTerraformStateError(w, err)
			return
		}
		if !found {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(body)
	case http.MethodPost:
		body, ok := readTerraformStateBody(w, r)
		if !ok {
			return
		}
		if err := s.store.WriteRunnerVMTerraformState(r.Context(), actor, reservationID, r.URL.Query().Get("ID"), body); err != nil {
			writeTerraformStateError(w, err)
			return
		}
		w.WriteHeader(http.StatusOK)
	case http.MethodDelete:
		if err := s.store.DeleteRunnerVMTerraformState(r.Context(), actor, reservationID, r.URL.Query().Get("ID")); err != nil {
			writeTerraformStateError(w, err)
			return
		}
		w.WriteHeader(http.StatusOK)
	case terraformLockMethod:
		body, ok := readTerraformStateBody(w, r)
		if !ok {
			return
		}
		lock, acquired, err := s.store.LockRunnerVMTerraformState(r.Context(), actor, reservationID, body)
		if err != nil {
			writeTerraformStateError(w, err)
			return
		}
		if !acquired {
			writeTerraformLock(w, http.StatusLocked, lock)
			return
		}
		w.WriteHeader(http.StatusOK)
	case terraformUnlockMethod:
		body, ok := readTerraformStateBody(w, r)
		if !ok {
			return
		}
		lock, released, err := s.store.UnlockRunnerVMTerraformState(r.Context(), actor, reservationID, body)
		if err != nil {
			writeTerraformStateError(w, err)
			return
		}
		if !released {
			writeTerraformLock(w, http.StatusConflict, lock)
			return
		}
		w.WriteHeader(http.StatusOK)
	default:
		w.Header().Set("Allow", terraformStateAllow)
		problem(w, http.StatusMethodNotAllowed, "invalid_argument", "unsupported Terraform state backend method", false)
	}
}

func readTerraformStateBody(w http.ResponseWriter, r *http.Request) ([]byte, bool) {
	r.Body = http.MaxBytesReader(w, r.Body, maxTerraformStateBody)
	defer r.Body.Close()
	body, err := io.ReadAll(r.Body)
	if err != nil {
		problem(w, http.StatusBadRequest, "invalid_argument", "Terraform state request body is too large or unreadable", false)
		return nil, false
	}
	return body, true
}

func writeTerraformLock(w http.ResponseWriter, status int, lock []byte) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_, _ = w.Write(lock)
}

func writeTerraformStateError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, store.ErrInvalid):
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid Terraform state request", false)
	case errors.Is(err, store.ErrNotFound):
		problem(w, http.StatusNotFound, "not_found", "runner reservation not found", false)
	case errors.Is(err, store.ErrConflict):
		problem(w, http.StatusConflict, "conflict", "Terraform state write conflicts with its current lock or lineage", false)
	default:
		problem(w, http.StatusServiceUnavailable, "unavailable", "Terraform state backend is unavailable", true)
	}
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The three pieces of the Terraform state backend that never touch the
// database: the body reader that bounds what a caller may push, the writer
// that hands a held lock back, and the one place a store failure is turned
// into an answer. Terraform retries on what these say, so the wording and
// the retryable flag are behaviour, not decoration.

// bodyThatFailsMidway yields some bytes and then refuses, which is how a
// truncated upload arrives: the request is well formed until it is not.
type bodyThatFailsMidway struct {
	remaining int
	err       error
}

func (b *bodyThatFailsMidway) Read(p []byte) (int, error) {
	if b.remaining <= 0 {
		return 0, b.err
	}
	n := min(len(p), b.remaining)
	for i := range n {
		p[i] = 'x'
	}
	b.remaining -= n
	return n, nil
}

func (b *bodyThatFailsMidway) Close() error { return nil }

func readBody(t *testing.T, body io.ReadCloser) ([]byte, bool, *httptest.ResponseRecorder) {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, "/v1/terraform/runner-states/x", body)
	response := httptest.NewRecorder()
	read, ok := readTerraformStateBody(response, request)
	return read, ok, response
}

// TestAStateBodyIsReadUpToItsBoundAndNoFurther pins the 16 MiB ceiling on
// both sides. Terraform state grows with the infrastructure it describes, so
// the bound has to be exact rather than approximate: a backend that refused a
// legitimate state one byte under would strand an operator with no way to
// write, and one that accepted an unbounded body would let any caller spend
// the plane's memory.
func TestAStateBodyIsReadUpToItsBoundAndNoFurther(t *testing.T) {
	atTheBound := strings.Repeat("s", maxTerraformStateBody)
	read, ok, response := readBody(t, io.NopCloser(strings.NewReader(atTheBound)))
	if !ok {
		t.Fatalf("a state body exactly at the bound was refused with %d", response.Code)
	}
	if len(read) != maxTerraformStateBody {
		t.Fatalf("read %d bytes at the bound, want %d", len(read), maxTerraformStateBody)
	}

	read, ok, response = readBody(t, io.NopCloser(strings.NewReader(atTheBound+"s")))
	if ok || read != nil {
		t.Fatalf("a state body one byte over the bound was accepted as %d bytes", len(read))
	}
	if response.Code != http.StatusBadRequest {
		t.Fatalf("over the bound status = %d, want 400", response.Code)
	}
	var body map[string]any
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body["code"] != "invalid_argument" || body["detail"] != "Terraform state request body is too large or unreadable" {
		t.Fatalf("over the bound answered %+v", body)
	}
	if body["retryable"] != false {
		t.Fatal("an oversized state body was called retryable, which would have Terraform push it again")
	}
}

// An empty body is a body. Terraform sends one on the verbs that carry no
// document, so reading zero bytes has to succeed rather than look like a
// truncated upload.
func TestAnEmptyStateBodyIsReadRatherThanRefused(t *testing.T) {
	read, ok, response := readBody(t, http.NoBody)
	if !ok {
		t.Fatalf("an empty body was refused with %d", response.Code)
	}
	if len(read) != 0 {
		t.Fatalf("an empty body read as %d bytes", len(read))
	}
	if response.Code != http.StatusOK || response.Body.Len() != 0 {
		t.Fatalf("reading an empty body wrote %d bytes and status %d", response.Body.Len(), response.Code)
	}
}

// A body that dies partway is refused in the same words as one that is too
// large, and nothing partial is handed back. Half a state file written as
// though it were whole is the one outcome this backend must never produce.
func TestATruncatedStateBodyHandsBackNothing(t *testing.T) {
	for name, failure := range map[string]error{
		"the connection dropped": io.ErrUnexpectedEOF,
		"the client went away":   errors.New("client disconnected"),
		"the reader was closed":  io.ErrClosedPipe,
	} {
		read, ok, response := readBody(t, &bodyThatFailsMidway{remaining: 4096, err: failure})
		if ok {
			t.Fatalf("%s: a truncated body was accepted as %d bytes", name, len(read))
		}
		if read != nil {
			t.Fatalf("%s: a refused read handed back %d bytes", name, len(read))
		}
		if response.Code != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", name, response.Code)
		}
	}
}

// The bytes are handed back exactly as they arrived. Nothing here parses the
// state, and it must not: Terraform owns that document's shape, and a backend
// that normalised it would change a lineage it does not understand.
func TestAStateBodyIsHandedOnVerbatim(t *testing.T) {
	for name, document := range map[string]string{
		"a state file":     `{"version":4,"lineage":"9d1f","serial":7}`,
		"not JSON at all":  "\x00\x01\x02 not a document",
		"trailing newline": "{}\n",
		"invalid UTF-8":    "\xff\xfe{}",
	} {
		read, ok, _ := readBody(t, io.NopCloser(strings.NewReader(document)))
		if !ok {
			t.Fatalf("%s was refused", name)
		}
		if string(read) != document {
			t.Fatalf("%s came back as %q", name, read)
		}
	}
}

// TestAHeldLockIsHandedBackUnreadAndUncached pins the two statuses Terraform
// distinguishes: 423 means someone else holds the lock, 409 means the unlock
// did not match. Both carry the lock document itself, because Terraform
// prints its holder to the operator, and both say no-store, because a cached
// lock is a lock that has already moved.
func TestAHeldLockIsHandedBackUnreadAndUncached(t *testing.T) {
	lock := []byte(`{"ID":"9d1f","Who":"runner@ra8ci","Created":"2026-09-29T00:00:00Z"}`)
	for name, status := range map[string]int{
		"another holder": http.StatusLocked,
		"a stale unlock": http.StatusConflict,
	} {
		response := httptest.NewRecorder()
		writeTerraformLock(response, status, lock)

		if response.Code != status {
			t.Fatalf("%s: status = %d, want %d", name, response.Code, status)
		}
		if got := response.Body.String(); got != string(lock) {
			t.Fatalf("%s: body = %q, want the lock verbatim", name, got)
		}
		if got := response.Header().Get("Content-Type"); got != "application/json" {
			t.Fatalf("%s: content type = %q", name, got)
		}
		if got := response.Header().Get("Cache-Control"); got != "no-store" {
			t.Fatalf("%s: cache control = %q, want no-store", name, got)
		}
	}
}

// A lock the store could not describe still answers with the status, headers
// and an empty body rather than inventing a holder. Terraform reports what it
// is given; a fabricated holder would send an operator after the wrong run.
func TestALockWithNothingToSayStillAnswersWithItsStatus(t *testing.T) {
	response := httptest.NewRecorder()
	writeTerraformLock(response, http.StatusLocked, nil)

	if response.Code != http.StatusLocked {
		t.Fatalf("status = %d, want 423", response.Code)
	}
	if response.Body.Len() != 0 {
		t.Fatalf("an absent lock wrote %q", response.Body.String())
	}
	if response.Header().Get("Cache-Control") != "no-store" {
		t.Fatal("an absent lock was cacheable")
	}
}

// TestEachStoreFailureIsTranslatedForTerraform pins the whole translation
// table at once. Terraform retries on the retryable one and stops on the
// others, so a mistranslation here is either a wedged apply or an infinite
// one.
func TestEachStoreFailureIsTranslatedForTerraform(t *testing.T) {
	for name, expected := range map[string]struct {
		err       error
		status    int
		code      string
		detail    string
		retryable bool
	}{
		"the request made no sense": {
			err: store.ErrInvalid, status: http.StatusBadRequest,
			code: "invalid_argument", detail: "invalid Terraform state request",
		},
		"no such reservation": {
			err: store.ErrNotFound, status: http.StatusNotFound,
			code: "not_found", detail: "runner reservation not found",
		},
		"the lock or lineage disagrees": {
			err: store.ErrConflict, status: http.StatusConflict,
			code: "conflict", detail: "Terraform state write conflicts with its current lock or lineage",
		},
		"the database could not answer": {
			err: errors.New("dial tcp: connection refused"), status: http.StatusServiceUnavailable,
			code: "unavailable", detail: "Terraform state backend is unavailable", retryable: true,
		},
		"the store said it is unavailable": {
			err: store.ErrUnavailable, status: http.StatusServiceUnavailable,
			code: "unavailable", detail: "Terraform state backend is unavailable", retryable: true,
		},
		"a denial that reached the wrong writer": {
			err: store.ErrDenied, status: http.StatusServiceUnavailable,
			code: "unavailable", detail: "Terraform state backend is unavailable", retryable: true,
		},
	} {
		response := httptest.NewRecorder()
		writeTerraformStateError(response, expected.err)

		if response.Code != expected.status {
			t.Fatalf("%s: status = %d, want %d", name, response.Code, expected.status)
		}
		var body map[string]any
		if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if body["code"] != expected.code || body["detail"] != expected.detail {
			t.Fatalf("%s answered %+v", name, body)
		}
		if body["retryable"] != expected.retryable {
			t.Fatalf("%s retryable = %v, want %v", name, body["retryable"], expected.retryable)
		}
		if got := response.Header().Get("Content-Type"); got != "application/problem+json" {
			t.Fatalf("%s: content type = %q", name, got)
		}
	}
}

// The translation reads the error's chain, not its text. A store error
// wrapped with the context of where it happened must still answer as itself,
// or the first caller to add a sentence of context turns every refusal into
// an unavailable the client retries forever.
func TestAWrappedStoreFailureIsStillTranslatedAsItself(t *testing.T) {
	for name, expected := range map[string]struct {
		err    error
		status int
	}{
		"wrapped once":  {err: fmt.Errorf("read runner state: %w", store.ErrNotFound), status: http.StatusNotFound},
		"wrapped twice": {err: fmt.Errorf("lock: %w", fmt.Errorf("write state: %w", store.ErrConflict)), status: http.StatusConflict},
		"joined":        {err: errors.Join(errors.New("after retry"), store.ErrInvalid), status: http.StatusBadRequest},
	} {
		response := httptest.NewRecorder()
		writeTerraformStateError(response, expected.err)
		if response.Code != expected.status {
			t.Fatalf("%s: status = %d, want %d", name, response.Code, expected.status)
		}
	}
}

// A nil error is not a success to report here. Nothing should call it with
// one, and if something does it must answer unavailable rather than 200 with
// an empty state, which Terraform would read as a fresh workspace and then
// plan a rebuild of infrastructure that already exists.
func TestNoErrorAtAllIsStillNotAnAnswer(t *testing.T) {
	response := httptest.NewRecorder()
	writeTerraformStateError(response, nil)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", response.Code)
	}
}

// TestAStateRefusalNeverQuotesWhatTheStoreSaid pins the one thing all of
// these share: the detail is chosen from a fixed set here, never taken from
// the error. A store error can carry a reservation ID, a query, or a
// connection string, and this backend answers callers who have not yet been
// authorized when the lookup fails.
func TestAStateRefusalNeverQuotesWhatTheStoreSaid(t *testing.T) {
	secret := "host=db.internal user=ra8ci password=hunter2 reservation=01890a2b"
	for name, err := range map[string]error{
		"a bare error":      errors.New(secret),
		"a wrapped invalid": fmt.Errorf("%s: %w", secret, store.ErrInvalid),
		"a wrapped missing": fmt.Errorf("%s: %w", secret, store.ErrNotFound),
		"a wrapped clash":   fmt.Errorf("%s: %w", secret, store.ErrConflict),
	} {
		response := httptest.NewRecorder()
		writeTerraformStateError(response, err)
		if strings.Contains(response.Body.String(), "hunter2") || strings.Contains(response.Body.String(), "db.internal") {
			t.Fatalf("%s leaked the store's own words: %s", name, response.Body.String())
		}
	}
}

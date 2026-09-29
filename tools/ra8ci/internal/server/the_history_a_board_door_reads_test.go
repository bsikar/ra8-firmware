// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The HIL observation history door answers a question about one board's own
// past, so every refusal it makes is decided before any history is read: the
// peer is authorized for this board, the catalog is present, the request
// parses, and the task named is a HIL definition for this board. None of
// that needs a database, and the door had no test at all before this one.
//
// The far side of the last check is not reachable here: the reviewed
// catalog declares no HIL task, so nothing can be named that reaches the
// store's optional history capability. What that leaves is every refusal,
// which is the part an operator meets.

func hilHistoryPlane(t *testing.T, f *fakeBoardStore, cat *catalog.Catalog) *http.ServeMux {
	t.Helper()
	mux := http.NewServeMux()
	if err := RegisterBoardRoutes(mux, f, nil, "bsikar/ra8-firmware", BoardPolicy{Catalog: cat}); err != nil {
		t.Fatal(err)
	}
	return mux
}

func askedForHistory(t *testing.T, mux *http.ServeMux, boardID, contentType, body string) answered {
	t.Helper()
	request := boardTestRequest(http.MethodPost, "/v1/boards/"+boardID+"/hil-observations", body)
	request.Header.Del("Content-Type")
	if contentType != "" {
		request.Header.Set("Content-Type", contentType)
	}
	response := httptest.NewRecorder()
	mux.ServeHTTP(response, request)

	result := answered{status: response.Code}
	if response.Body.Len() > 0 {
		if err := json.Unmarshal(response.Body.Bytes(), &result.body); err != nil {
			t.Fatalf("history answered a body that is not JSON: %q", response.Body.String())
		}
	}
	return result
}

func reviewedCatalog(t *testing.T) *catalog.Catalog {
	t.Helper()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	return cat
}

// anOrdinaryTask is a task the reviewed catalog really declares that is not
// a HIL task. Discovered rather than hardcoded: the catalog is the source of
// truth for what exists, and a name written by hand goes stale the day it is
// renamed.
func anOrdinaryTask(t *testing.T, cat *catalog.Catalog) catalog.Task {
	t.Helper()
	for _, name := range cat.Names() {
		if task, found := cat.Task(name); found && task.Scope != "hil" {
			return task
		}
	}
	t.Fatal("the reviewed catalog declares no ordinary task")
	return catalog.Task{}
}

// TestHILHistoryAuthorizesTheBoardBeforeItReadsAnything pins the first gate.
// A denial is audited under this door's own action name, which is what lets
// an operator tell a refused history request apart from a refused take, and
// an audit trail that cannot be written closes the door rather than opening
// it.
func TestHILHistoryAuthorizesTheBoardBeforeItReadsAnything(t *testing.T) {
	cat := reviewedCatalog(t)

	denied := &fakeBoardStore{authorizeErr: store.ErrDenied}
	result := askedForHistory(t, hilHistoryPlane(t, denied, cat), "ek-ra8d2", "application/json", `{"task_name":"whatever"}`)
	if result.status != http.StatusNotFound {
		t.Fatalf("a denied peer got %d, want 404", result.status)
	}
	if denied.audits != 1 || denied.action != "board.hil.history" {
		t.Fatalf("denial audited as %d %q", denied.audits, denied.action)
	}

	unavailable := &fakeBoardStore{authorizeErr: store.ErrUnavailable}
	if got := askedForHistory(t, hilHistoryPlane(t, unavailable, cat), "ek-ra8d2", "application/json", `{}`); got.status != http.StatusServiceUnavailable {
		t.Fatalf("an unavailable authorizer got %d, want 503", got.status)
	}

	failing := &fakeBoardStore{authorizeErr: store.ErrDenied, auditErr: store.ErrUnavailable}
	if got := askedForHistory(t, hilHistoryPlane(t, failing, cat), "ek-ra8d2", "application/json", `{}`); got.status != http.StatusServiceUnavailable {
		t.Fatalf("an unwritable audit trail got %d, want the closed door's 503", got.status)
	}
}

// A board ID that is not a board ID never reaches the store at all, not even
// to be audited: there is nothing yet worth naming in the record.
func TestHILHistoryRefusesABoardIDThatIsNotOne(t *testing.T) {
	f := &fakeBoardStore{}
	result := askedForHistory(t, hilHistoryPlane(t, f, reviewedCatalog(t)), "ek~ra8d2", "application/json", `{}`)
	if result.status != http.StatusBadRequest || result.body["detail"] != "invalid board ID" {
		t.Fatalf("answered %d %+v", result.status, result.body)
	}
	if f.audits != 0 {
		t.Fatalf("an unusable board ID reached the audit trail %d times", f.audits)
	}
}

// TestHILHistoryWithoutACatalogSaysSoBeforeItReadsTheRequest pins the
// ordering. A plane registered with no catalog cannot judge any task name,
// so it says the catalog is missing rather than blaming the caller's body:
// this request carries neither a JSON type nor JSON, and the answer is still
// about the catalog.
func TestHILHistoryWithoutACatalogSaysSoBeforeItReadsTheRequest(t *testing.T) {
	result := askedForHistory(t, hilHistoryPlane(t, &fakeBoardStore{}, nil), "ek-ra8d2", "text/plain", `not even JSON`)
	if result.status != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", result.status)
	}
	if result.body["detail"] != "HIL catalog is not configured" {
		t.Fatalf("answered %+v, want the catalog's own refusal ahead of the body's", result.body)
	}
}

// TestHILHistoryJudgesTheRequestItWasSent walks the shared board decoder
// through this door and then the task check behind it. The task refusal is
// one wording for four different mistakes, which is deliberate: a caller
// asking about another board's task learns no more than that it was refused.
func TestHILHistoryJudgesTheRequestItWasSent(t *testing.T) {
	cat := reviewedCatalog(t)
	ordinary := anOrdinaryTask(t, cat)

	for name, refused := range map[string]struct {
		contentType string
		body        string
		status      int
		detail      string
	}{
		"no content type": {
			body: `{}`, status: http.StatusUnsupportedMediaType,
			detail: "content type must be application/json",
		},
		"a plain text body": {
			contentType: "text/plain", body: `{}`, status: http.StatusUnsupportedMediaType,
			detail: "content type must be application/json",
		},
		"a content type that does not parse": {
			contentType: "application/", body: `{}`, status: http.StatusUnsupportedMediaType,
			detail: "content type must be application/json",
		},
		"a document that does not parse": {
			contentType: "application/json", body: `{"task_name":`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a field this door does not know": {
			contentType: "application/json", body: `{"task_name":"x","since":"yesterday"}`,
			status: http.StatusBadRequest, detail: "invalid board request",
		},
		"a second document after the first": {
			contentType: "application/json", body: `{"task_name":"x"} {"task_name":"y"}`,
			status: http.StatusBadRequest, detail: "trailing board request data",
		},
		"a task the catalog does not declare": {
			contentType: "application/json", body: `{"task_name":"no-such-task"}`,
			status: http.StatusBadRequest, detail: "task is not a HIL definition for this board",
		},
		"a task that is not a HIL task": {
			contentType: "application/json", body: `{"task_name":"` + ordinary.Name + `"}`,
			status: http.StatusBadRequest, detail: "task is not a HIL definition for this board",
		},
		"no task at all": {
			contentType: "application/json", body: `{}`,
			status: http.StatusBadRequest, detail: "task is not a HIL definition for this board",
		},
	} {
		result := askedForHistory(t, hilHistoryPlane(t, &fakeBoardStore{}, cat), "ek-ra8d2", refused.contentType, refused.body)
		if result.status != refused.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refused.status)
		}
		if result.body["detail"] != refused.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, refused.detail)
		}
	}
}

// A charset on the media type is fine here. The board decoder parses the
// header rather than comparing it, which is a third spelling of the same
// decision across this plane: run creation matches a prefix, the offline
// ingest compares the whole header, and the board doors parse it.
func TestHILHistoryTakesAParameterisedJSONType(t *testing.T) {
	result := askedForHistory(t, hilHistoryPlane(t, &fakeBoardStore{}, reviewedCatalog(t)),
		"ek-ra8d2", "application/json; charset=utf-8", `{"task_name":"no-such-task"}`)

	if result.status != http.StatusBadRequest || result.body["detail"] != "task is not a HIL definition for this board" {
		t.Fatalf("answered %d %+v, want the request to have been read", result.status, result.body)
	}
}

// The board decoder holds a 128 KiB ceiling, and a body past it is refused
// as an unreadable request rather than accepted or left to exhaust memory.
func TestHILHistoryRefusesARequestPastTheBoardCeiling(t *testing.T) {
	oversized := `{"task_name":"` + strings.Repeat("t", (128<<10)+1) + `"}`
	result := askedForHistory(t, hilHistoryPlane(t, &fakeBoardStore{}, reviewedCatalog(t)),
		"ek-ra8d2", "application/json", oversized)

	if result.status != http.StatusBadRequest || result.body["detail"] != "invalid board request" {
		t.Fatalf("answered %d %+v", result.status, result.body)
	}
}

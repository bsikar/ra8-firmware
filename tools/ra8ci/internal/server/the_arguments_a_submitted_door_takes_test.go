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

// The three doors that take a repository from the caller rather than from a
// path: run creation, offline ingest and the slow report. Each judges its
// arguments before it authorizes anything, which matters because a denial
// writes the caller's own repository text into the audit trail as its target.
// Everything below is answered without the store.

// refusingPlane authorizes nobody: MTLSAuthorizer turns away a request with
// no verified peer before it consults anything, and the denial lands in a
// recorder rather than the database. That makes "got past the argument gate"
// observable as a denial, which is how both sides of a bound can be pinned
// without a live store.
func refusingPlane(t *testing.T) (*Server, *recordingAuditor) {
	t.Helper()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	auditor := &recordingAuditor{}
	return &Server{
		store:   &store.Store{},
		audit:   auditor,
		catalog: cat,
		auth:    MTLSAuthorizer{Store: &store.Store{}},
	}, auditor
}

func submitted(t *testing.T, handle func(*Server, http.ResponseWriter, *http.Request), target, contentType, key, body string) (answered, *recordingAuditor) {
	t.Helper()
	api, auditor := refusingPlane(t)
	request := httptest.NewRequest(http.MethodPost, target, strings.NewReader(body))
	if contentType != "" {
		request.Header.Set("Content-Type", contentType)
	}
	if key != "" {
		request.Header.Set("Idempotency-Key", key)
	}
	response := httptest.NewRecorder()
	handle(api, response, request)

	result := answered{status: response.Code}
	if response.Body.Len() > 0 {
		if err := json.Unmarshal(response.Body.Bytes(), &result.body); err != nil {
			t.Fatalf("%s answered a body that is not JSON: %q", target, response.Body.String())
		}
	}
	return result, auditor
}

// createRunDocument is a submission whose only interesting field is the
// repository, as the caller spells it. Note "repo": createRequest names the
// field differently from the spooled offline record, which says
// "repository", and a document written with the other door's spelling is
// refused by the decoder rather than by the repository check.
func createRunDocument(repository string) string {
	return `{"trigger":"manual","catalog_digest":"` + strings.Repeat("c", 64) +
		`","source":{"repo":"` + repository + `","branch":"main","commit":"` + strings.Repeat("a", 40) +
		`","snapshot_sha256":"` + strings.Repeat("b", 64) + `"},"tasks":[{"key":"build","name":"build"}]}`
}

// TestRunCreationJudgesEveryArgumentBeforeItAuthorizes pins the whole front
// of createRun. None of these refusals may reach the authorizer, and the
// empty audit trail is the assertion: a door that authorized first would
// write the caller's unjudged text into the record before deciding it was
// nonsense.
func TestRunCreationJudgesEveryArgumentBeforeItAuthorizes(t *testing.T) {
	oversized := strings.Repeat("x", (1<<20)+1)
	for name, refused := range map[string]struct {
		contentType string
		key         string
		body        string
		status      int
		detail      string
	}{
		"no content type": {
			key: "k", body: "{}", status: http.StatusUnsupportedMediaType,
			detail: "content type must be application/json",
		},
		"a form post": {
			contentType: "application/x-www-form-urlencoded", key: "k", body: "{}",
			status: http.StatusUnsupportedMediaType, detail: "content type must be application/json",
		},
		"no idempotency key": {
			contentType: "application/json", body: "{}", status: http.StatusBadRequest,
			detail: "Idempotency-Key is required (1..256 bytes)",
		},
		"an idempotency key past its bound": {
			contentType: "application/json", key: strings.Repeat("k", 257), body: "{}",
			status: http.StatusBadRequest, detail: "Idempotency-Key is required (1..256 bytes)",
		},
		"a body past the megabyte": {
			contentType: "application/json", key: "k", body: oversized,
			status: http.StatusBadRequest, detail: "request body exceeds limit or is unreadable",
		},
		"a document that does not parse": {
			contentType: "application/json", key: "k", body: `{"trigger":`,
			status: http.StatusBadRequest, detail: "invalid run request",
		},
		"a field the plane does not know": {
			contentType: "application/json", key: "k", body: `{"trigger":"manual","priority":9}`,
			status: http.StatusBadRequest, detail: "invalid run request",
		},
		"a second document after the first": {
			contentType: "application/json", key: "k", body: createRunDocument("bsikar/ra8-firmware") + ` {"trigger":"manual"}`,
			status: http.StatusBadRequest, detail: "trailing JSON data",
		},
		"no repository at all": {
			contentType: "application/json", key: "k", body: createRunDocument(""),
			status: http.StatusBadRequest, detail: "run request states no usable repository",
		},
		"a repository carrying a control character": {
			contentType: "application/json", key: "k", body: `{"source":{"repo":"bsikar/ra8\u0000firmware"}}`,
			status: http.StatusBadRequest, detail: "run request states no usable repository",
		},
		"a repository carrying a newline": {
			contentType: "application/json", key: "k", body: `{"source":{"repo":"bsikar/ra8\nfirmware"}}`,
			status: http.StatusBadRequest, detail: "run request states no usable repository",
		},
		"the other door's spelling of the repository": {
			contentType: "application/json", key: "k", body: `{"source":{"repository":"bsikar/ra8-firmware"}}`,
			status: http.StatusBadRequest, detail: "invalid run request",
		},
		"a repository past its length": {
			contentType: "application/json", key: "k", body: createRunDocument(strings.Repeat("r", maxRepositoryLength+1)),
			status: http.StatusBadRequest, detail: "run request states no usable repository",
		},
	} {
		result, auditor := submitted(t, (*Server).createRun, "/v1/runs", refused.contentType, refused.key, refused.body)
		if result.status != refused.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refused.status)
		}
		if result.body["detail"] != refused.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, refused.detail)
		}
		if len(auditor.records) != 0 {
			t.Fatalf("%s reached the audit trail before it was understood: %+v", name, auditor.records)
		}
	}
}

// And the far side of that gate: a request whose arguments all hold is
// carried into authorization, where this plane refuses it and writes the
// repository it was given as the target. That is what makes the ordering
// above worth having, and it is the only way to show the gate is a gate
// rather than a wall.
func TestARunRequestThatHoldsIsCarriedIntoAuthorization(t *testing.T) {
	result, auditor := submitted(t, (*Server).createRun, "/v1/runs",
		"application/json", "k", createRunDocument("bsikar/ra8-firmware"))

	if result.status != http.StatusNotFound {
		t.Fatalf("status = %d, want the denial's 404", result.status)
	}
	if len(auditor.records) != 1 {
		t.Fatalf("wrote %d denial records, want 1", len(auditor.records))
	}
	if auditor.records[0].action != "run.create" || auditor.records[0].target != "bsikar/ra8-firmware" {
		t.Fatalf("audited %+v", auditor.records[0])
	}
}

// TestTheIdempotencyKeyBoundIsExact pins both edges. One byte is a key; 256
// is a key; 257 is not. A door that read the bound as exclusive would turn
// away a submitter whose key is exactly as long as the header allows.
func TestTheIdempotencyKeyBoundIsExact(t *testing.T) {
	for name, key := range map[string]string{
		"one byte":       "k",
		"at the ceiling": strings.Repeat("k", 256),
	} {
		result, _ := submitted(t, (*Server).createRun, "/v1/runs", "application/json", key, `{"trigger":`)
		if result.body["detail"] != "invalid run request" {
			t.Fatalf("%s was not carried past the key check: %+v", name, result.body)
		}
	}
	result, _ := submitted(t, (*Server).createRun, "/v1/runs", "application/json", strings.Repeat("k", 257), `{"trigger":`)
	if result.body["detail"] != "Idempotency-Key is required (1..256 bytes)" {
		t.Fatalf("a 257 byte key was accepted: %+v", result.body)
	}
}

// A body exactly at the megabyte is read; the refusal is for what is past it.
// Pinned by the wording: at the bound the request is judged on its content,
// over the bound it never gets that far.
func TestARunBodyAtTheMegabyteIsReadAndJudgedOnItsContent(t *testing.T) {
	atTheBound := strings.Repeat("x", 1<<20)
	result, _ := submitted(t, (*Server).createRun, "/v1/runs", "application/json", "k", atTheBound)
	if result.body["detail"] != "invalid run request" {
		t.Fatalf("a body at the bound answered %+v, want the decoder's refusal", result.body)
	}
}

// TestOfflineIngestNamesItsMediaTypeExactlyWhereRunCreationTakesAPrefix pins
// a real divergence between two doors that look alike. Run creation matches a
// prefix, so a charset is fine; offline ingest compares the whole header, so
// the same request is refused. Measured, not assumed, and pinned here so the
// next reader knows which door they are changing.
func TestOfflineIngestNamesItsMediaTypeExactlyWhereRunCreationTakesAPrefix(t *testing.T) {
	const withCharset = "application/json; charset=utf-8"

	created, _ := submitted(t, (*Server).createRun, "/v1/runs", withCharset, "k", `{"trigger":`)
	if created.status != http.StatusBadRequest || created.body["detail"] != "invalid run request" {
		t.Fatalf("run creation refused a parameterised JSON type: %d %+v", created.status, created.body)
	}

	ingested, _ := submitted(t, (*Server).ingestOffline, "/v1/local-runs/sync", withCharset, "", `{}`)
	if ingested.status != http.StatusUnsupportedMediaType {
		t.Fatalf("offline ingest accepted a parameterised JSON type: %d %+v", ingested.status, ingested.body)
	}
}

// TestOfflineIngestJudgesItsRecordBeforeItAuthorizes is the same rule as run
// creation, on the door that takes a spooled record from a runner that was
// offline. Its body ceiling is smaller, and its refusals are worded for the
// record rather than the request.
func TestOfflineIngestJudgesItsRecordBeforeItAuthorizes(t *testing.T) {
	for name, refused := range map[string]struct {
		contentType string
		body        string
		status      int
		detail      string
	}{
		"a plain text record": {
			contentType: "text/plain", body: "{}", status: http.StatusUnsupportedMediaType,
			detail: "content type must be application/json",
		},
		"a record past 256 KiB": {
			contentType: "application/json", body: strings.Repeat("x", (256<<10)+1),
			status: http.StatusBadRequest, detail: "offline record exceeds limit or is unreadable",
		},
		"a record that does not parse": {
			contentType: "application/json", body: `{"source":`,
			status: http.StatusBadRequest, detail: "invalid offline record",
		},
		"a field the spool does not know": {
			contentType: "application/json", body: `{"not_a_spool_field":1}`,
			status: http.StatusBadRequest, detail: "invalid offline record",
		},
		"a second record after the first": {
			contentType: "application/json", body: `{} {}`,
			status: http.StatusBadRequest, detail: "trailing JSON data",
		},
		"a record naming no repository": {
			contentType: "application/json", body: `{}`,
			status: http.StatusBadRequest, detail: "offline record states no usable repository",
		},
		"a record whose repository carries a control character": {
			contentType: "application/json", body: `{"source":{"repository":"bsikar/ra8\u0001firmware"}}`,
			status: http.StatusBadRequest, detail: "offline record states no usable repository",
		},
	} {
		result, auditor := submitted(t, (*Server).ingestOffline, "/v1/local-runs/sync", refused.contentType, "", refused.body)
		if result.status != refused.status {
			t.Fatalf("%s: status = %d, want %d", name, result.status, refused.status)
		}
		if result.body["detail"] != refused.detail {
			t.Fatalf("%s answered %+v, want detail %q", name, result.body, refused.detail)
		}
		if len(auditor.records) != 0 {
			t.Fatalf("%s reached the audit trail before it was understood: %+v", name, auditor.records)
		}
	}
}

func reported(t *testing.T, query string) (answered, *recordingAuditor) {
	t.Helper()
	api, auditor := refusingPlane(t)
	response := httptest.NewRecorder()
	api.slowReport(response, httptest.NewRequest(http.MethodGet, "/v1/reports/slow?"+query, nil))

	result := answered{status: response.Code}
	if response.Body.Len() > 0 {
		if err := json.Unmarshal(response.Body.Bytes(), &result.body); err != nil {
			t.Fatalf("%q answered a body that is not JSON: %q", query, response.Body.String())
		}
	}
	return result, auditor
}

// TestTheSlowReportTakesExactlyItsThreeArguments pins the arity rule, which
// is stricter than it looks: three keys, one value each. A repeated parameter
// is refused rather than resolved to the first, because two windows in one
// request means the caller and the plane disagree about what was asked.
func TestTheSlowReportTakesExactlyItsThreeArguments(t *testing.T) {
	for name, query := range map[string]string{
		"nothing at all":        "",
		"only a repository":     "repository=bsikar/ra8-firmware",
		"a missing limit":       "repository=bsikar/ra8-firmware&window_seconds=3600",
		"one argument too many": "repository=bsikar/ra8-firmware&window_seconds=3600&limit=10&offset=5",
		"a repeated window":     "repository=bsikar/ra8-firmware&window_seconds=3600&window_seconds=60&limit=10",
		"a repeated limit":      "repository=bsikar/ra8-firmware&window_seconds=3600&limit=10&limit=20",
	} {
		result, auditor := reported(t, query)
		if result.status != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", name, result.status)
		}
		if result.body["detail"] != "repository, window_seconds, and limit are required" {
			t.Fatalf("%s answered %+v", name, result.body)
		}
		if len(auditor.records) != 0 {
			t.Fatalf("%s was audited before it was understood: %+v", name, auditor.records)
		}
	}
}

// TestTheSlowReportWindowAndLimitBoundsAreExact walks both edges of both
// bounds. The accepted side is shown by the request reaching authorization
// and being denied there, which is the only evidence available without a
// store, and the better evidence anyway: it says the argument gate passed it
// on rather than merely that nothing complained.
func TestTheSlowReportWindowAndLimitBoundsAreExact(t *testing.T) {
	const year = 365 * 24 * 60 * 60

	for name, query := range map[string]string{
		"a window of zero seconds":      "repository=bsikar/ra8-firmware&window_seconds=0&limit=10",
		"a negative window":             "repository=bsikar/ra8-firmware&window_seconds=-1&limit=10",
		"a window past the year":        "repository=bsikar/ra8-firmware&window_seconds=31536001&limit=10",
		"a window that is not a number": "repository=bsikar/ra8-firmware&window_seconds=3600s&limit=10",
		"a limit of zero":               "repository=bsikar/ra8-firmware&window_seconds=3600&limit=0",
		"a limit past five hundred":     "repository=bsikar/ra8-firmware&window_seconds=3600&limit=501",
		"a limit that is not a number":  "repository=bsikar/ra8-firmware&window_seconds=3600&limit=ten",
		"an unusable repository":        "repository=&window_seconds=3600&limit=10",
	} {
		result, auditor := reported(t, query)
		if result.status != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", name, result.status)
		}
		if result.body["detail"] != "invalid slow report window, repository, or limit" {
			t.Fatalf("%s answered %+v", name, result.body)
		}
		if len(auditor.records) != 0 {
			t.Fatalf("%s was audited before it was understood: %+v", name, auditor.records)
		}
	}

	for name, query := range map[string]string{
		"one second and one row":   "repository=bsikar/ra8-firmware&window_seconds=1&limit=1",
		"a full year of rows":      "repository=bsikar/ra8-firmware&window_seconds=31536000&limit=500",
		"a year is exactly a year": "repository=bsikar/ra8-firmware&window_seconds=" + itoa(year) + "&limit=250",
	} {
		result, auditor := reported(t, query)
		if result.status != http.StatusNotFound {
			t.Fatalf("%s: status = %d, want the denial's 404, so the bound accepted it", name, result.status)
		}
		if len(auditor.records) != 1 || auditor.records[0].action != "report.slow" ||
			auditor.records[0].target != "bsikar/ra8-firmware" {
			t.Fatalf("%s audited %+v", name, auditor.records)
		}
	}
}

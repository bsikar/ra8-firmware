// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

const reconcilerHead = "0123456789abcdef0123456789abcdef01234567"

type commitCheckRunServer struct {
	mu           sync.Mutex
	tokenBodies  []installationTokenRequest
	methods      []string
	paths        []string
	queries      []string
	listStatus   int
	listPages    []string
	listRequests int
}

func newCheckRunReconciler(t *testing.T) (*CheckRunReconciler, *commitCheckRunServer) {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	if err := os.WriteFile(keyPath, keyPEM, 0600); err != nil {
		t.Fatalf("write key: %v", err)
	}
	state := &commitCheckRunServer{
		listStatus: http.StatusOK,
		listPages:  []string{`{"total_count":0,"check_runs":[]}`},
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		state.mu.Lock()
		defer state.mu.Unlock()
		state.methods = append(state.methods, r.Method)
		state.paths = append(state.paths, r.URL.Path)
		state.queries = append(state.queries, r.URL.RawQuery)
		if strings.HasSuffix(r.URL.Path, "/access_tokens") {
			var body installationTokenRequest
			_ = json.NewDecoder(r.Body).Decode(&body)
			state.tokenBodies = append(state.tokenBodies, body)
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour)})
			return
		}
		page := state.listRequests
		state.listRequests++
		w.WriteHeader(state.listStatus)
		if state.listStatus == http.StatusOK && page < len(state.listPages) {
			_, _ = w.Write([]byte(state.listPages[page]))
		}
	}))
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{inner: transport, host: strings.TrimPrefix(server.URL, "https://")}}
	reconciler, err := NewCheckRunReconciler(CheckRunReconcilerConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatalf("new reconciler: %v", err)
	}
	return reconciler, state
}

func (s *commitCheckRunServer) serveListing(status int, pages ...string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.listStatus, s.listPages, s.listRequests = status, pages, 0
}

func (s *commitCheckRunServer) observed() ([]string, []string, []string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string{}, s.methods...), append([]string{}, s.paths...), append([]string{}, s.queries...)
}

func publishedRunJSON(id int64, name, head, status, conclusion, title string) string {
	return fmt.Sprintf(`{"id":%d,"name":%q,"head_sha":%q,"status":%q,"conclusion":%q,"output":{"title":%q}}`,
		id, name, head, status, conclusion, title)
}

func reconcilerTaskNames(t *testing.T) (string, string) {
	t.Helper()
	names := catalogNames(t)
	if len(names) < 2 {
		t.Skip("catalog carries fewer than two tasks")
	}
	return names[0], names[1]
}

// The commit's runs come back with the mode each name carries, the conclusion
// and title verbatim, sorted so two reads of one commit produce one document.
func TestPublishedRunsReportWhatThisPlaneWroteOnTheCommit(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, second := reconcilerTaskNames(t)
	shadowName, err := CheckRunName(ModeShadow, second)
	if err != nil {
		t.Fatalf("shadow name: %v", err)
	}
	authoritativeName, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("authoritative name: %v", err)
	}
	server.serveListing(http.StatusOK, `{"total_count":2,"check_runs":[`+
		publishedRunJSON(2, shadowName, reconcilerHead, "completed", "neutral", "shadow: would report failure")+`,`+
		publishedRunJSON(1, authoritativeName, reconcilerHead, "completed", "success", "ok")+`]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	if published.HeadSHA != reconcilerHead || len(published.Runs) != 2 {
		t.Fatalf("published = %+v", published)
	}
	if published.Runs[0].Name >= published.Runs[1].Name {
		t.Fatalf("runs are not sorted by name: %+v", published.Runs)
	}
	for _, run := range published.Runs {
		switch run.Name {
		case shadowName:
			if run.Mode != ModeShadow || run.Conclusion != "neutral" || run.Title != "shadow: would report failure" {
				t.Fatalf("shadow run = %+v", run)
			}
		case authoritativeName:
			if run.Mode != ModeAuthoritative || run.Conclusion != "success" || run.ID != 1 {
				t.Fatalf("authoritative run = %+v", run)
			}
		default:
			t.Fatalf("unexpected run %+v", run)
		}
	}
}

// A run under a name this plane does not publish is not evidence about what
// this plane wrote, and is left out.
func TestForeignCheckRunsAreNotReportedAsOurs(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, _ := reconcilerTaskNames(t)
	mine, err := CheckRunName(ModeShadow, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	server.serveListing(http.StatusOK, `{"total_count":4,"check_runs":[`+
		publishedRunJSON(1, "build (ubuntu-latest)", reconcilerHead, "completed", "success", "")+`,`+
		publishedRunJSON(2, "CodeQL", reconcilerHead, "completed", "success", "")+`,`+
		publishedRunJSON(3, "ra8ci-lint", reconcilerHead, "completed", "failure", "")+`,`+
		publishedRunJSON(4, mine, reconcilerHead, "completed", "neutral", "")+`]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	if len(published.Runs) != 1 || published.Runs[0].Name != mine {
		t.Fatalf("published = %+v", published.Runs)
	}
}

// An unreadable listing is never a commit carrying no check runs: reading it
// as one is exactly the blind repeat this reader replaces.
func TestAnUnreadableListingIsNeverACommitWithoutRuns(t *testing.T) {
	for _, status := range []int{http.StatusNotFound, http.StatusForbidden, http.StatusUnauthorized, http.StatusInternalServerError} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			reconciler, server := newCheckRunReconciler(t)
			server.serveListing(status, "")
			published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
			if !errors.Is(err, ErrPublishedCheckRunsUnreadable) {
				t.Fatalf("err = %v", err)
			}
			if len(published.Runs) != 0 || published.HeadSHA != "" {
				t.Fatalf("published = %+v", published)
			}
		})
	}
}

// A commit GitHub answered for that carries none of our runs is an answer, and
// is distinguishable from the refusal above.
func TestACommitWithNoRunsOfOursIsAnAnswer(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	server.serveListing(http.StatusOK, `{"total_count":0,"check_runs":[]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	if len(published.Runs) != 0 || published.HeadSHA != reconcilerHead {
		t.Fatalf("published = %+v", published)
	}
}

// A run that has not finished is the shape an uncertain write usually takes,
// so it is reported with its empty conclusion rather than hidden.
func TestAnUnfinishedRunIsReported(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, _ := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	server.serveListing(http.StatusOK, `{"total_count":1,"check_runs":[`+
		publishedRunJSON(9, name, reconcilerHead, "in_progress", "", "")+`]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	if len(published.Runs) != 1 || published.Runs[0].Status != "in_progress" || published.Runs[0].Conclusion != "" {
		t.Fatalf("published = %+v", published.Runs)
	}
}

// Two runs under one name is the state a repeated write leaves behind. Both
// are reported, oldest identifier first, and Named finds them together.
func TestARepeatedWriteIsVisibleAsTwoRuns(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, _ := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeShadow, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	server.serveListing(http.StatusOK, `{"total_count":2,"check_runs":[`+
		publishedRunJSON(88, name, reconcilerHead, "completed", "neutral", "second")+`,`+
		publishedRunJSON(12, name, reconcilerHead, "completed", "neutral", "first")+`]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	named := published.Named(name)
	if len(named) != 2 || named[0].ID != 12 || named[1].ID != 88 {
		t.Fatalf("named = %+v", named)
	}
	if len(published.Named(name+"-other")) != 0 {
		t.Fatalf("Named matched a different name")
	}
}

// A listing answering about another commit cannot be reconciled against this
// one.
func TestAListingAboutAnotherCommitIsRefused(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, _ := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeShadow, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	other := strings.Repeat("a", 40)
	server.serveListing(http.StatusOK, `{"total_count":1,"check_runs":[`+
		publishedRunJSON(4, name, other, "completed", "neutral", "")+`]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if !errors.Is(err, ErrPublishedCheckRunsUnreadable) {
		t.Fatalf("err = %v", err)
	}
	if len(published.Runs) != 0 {
		t.Fatalf("published = %+v", published)
	}
}

// The commit is named by its full SHA, refused before a token is minted: a
// branch answers about whatever it points at when the listing is made.
func TestTheCommitIsNamedBySHABeforeATokenIsMinted(t *testing.T) {
	for _, ref := range []string{"", "main", "refs/heads/main", "0123456", strings.Repeat("a", 41),
		" " + reconcilerHead, reconcilerHead + "\n", "../" + reconcilerHead, "0123456789abcdef0123456789abcdef0123456g"} {
		t.Run(fmt.Sprintf("%q", ref), func(t *testing.T) {
			reconciler, server := newCheckRunReconciler(t)
			_, err := reconciler.PublishedRuns(context.Background(), ref)
			if !errors.Is(err, ErrInvalidCheckRunSHA) {
				t.Fatalf("err = %v", err)
			}
			methods, _, _ := server.observed()
			if len(methods) != 0 {
				t.Fatalf("requests made for %q: %v", ref, methods)
			}
		})
	}
}

// Discovering what was published cannot change it: every request but the token
// mint is a GET, and the listing is asked for by page.
func TestReconcilingOnlyReads(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	if _, err := reconciler.PublishedRuns(context.Background(), reconcilerHead); err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	methods, paths, queries := server.observed()
	listings := 0
	for index, path := range paths {
		if strings.HasSuffix(path, "/access_tokens") {
			continue
		}
		listings++
		if methods[index] != http.MethodGet {
			t.Fatalf("listing reached with %s", methods[index])
		}
		if !strings.Contains(path, "/commits/"+reconcilerHead+"/check-runs") {
			t.Fatalf("listing path = %s", path)
		}
		if !strings.Contains(queries[index], "per_page=100") || !strings.Contains(queries[index], "page=1") {
			t.Fatalf("listing query = %s", queries[index])
		}
	}
	if listings != 1 {
		t.Fatalf("listing requests = %d", listings)
	}
}

// Its own narrow token: reading what was published never carries the write the
// publisher holds.
func TestTheReconcilerReadsChecksOnOneRepository(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	if _, err := reconciler.PublishedRuns(context.Background(), reconcilerHead); err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if len(server.tokenBodies) == 0 {
		t.Fatal("no installation token minted")
	}
	minted := server.tokenBodies[0]
	if len(minted.Repositories) != 1 || minted.Repositories[0] != "ra8-firmware" {
		t.Fatalf("token repositories = %v", minted.Repositories)
	}
	if len(minted.Permissions) != 1 || minted.Permissions["checks"] != "read" {
		t.Fatalf("token permissions = %v", minted.Permissions)
	}
}

// Pages are concatenated, and a commit carrying more runs than this reader
// pages through is refused rather than answered from part of the listing.
func TestPagingIsConcatenatedAndBounded(t *testing.T) {
	first, second := reconcilerTaskNames(t)
	firstName, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	secondName, err := CheckRunName(ModeAuthoritative, second)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	t.Run("concatenated", func(t *testing.T) {
		reconciler, server := newCheckRunReconciler(t)
		server.serveListing(http.StatusOK,
			`{"total_count":101,"check_runs":[`+publishedRunJSON(1, firstName, reconcilerHead, "completed", "success", "")+`]}`,
			`{"total_count":101,"check_runs":[`+publishedRunJSON(2, secondName, reconcilerHead, "completed", "failure", "")+`]}`)
		published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
		if err != nil {
			t.Fatalf("PublishedRuns: %v", err)
		}
		if len(published.Runs) != 2 {
			t.Fatalf("published = %+v", published.Runs)
		}
	})
	t.Run("bounded", func(t *testing.T) {
		reconciler, server := newCheckRunReconciler(t)
		pages := make([]string, maxPublishedCheckRunPages+1)
		for index := range pages {
			pages[index] = `{"total_count":100000,"check_runs":[` +
				publishedRunJSON(int64(index+1), firstName, reconcilerHead, "completed", "success", "") + `]}`
		}
		server.serveListing(http.StatusOK, pages...)
		published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
		if !errors.Is(err, ErrPublishedCheckRunsTooMany) {
			t.Fatalf("err = %v", err)
		}
		if len(published.Runs) != 0 {
			t.Fatalf("published = %+v", published)
		}
	})
}

// A listing document this reader cannot read is refused, never treated as an
// empty commit.
func TestAnUnreadableListingDocumentIsRefused(t *testing.T) {
	for name, page := range map[string]string{
		"not an object":  `["check_runs"]`,
		"negative total": `{"total_count":-1,"check_runs":[]}`,
		"truncated":      `{"total_count":1,"check_runs":[`,
	} {
		t.Run(name, func(t *testing.T) {
			reconciler, server := newCheckRunReconciler(t)
			server.serveListing(http.StatusOK, page)
			if _, err := reconciler.PublishedRuns(context.Background(), reconcilerHead); !errors.Is(err, ErrPublishedCheckRunsUnreadable) {
				t.Fatalf("err = %v", err)
			}
		})
	}
}

// The listing carries each run's external identifier verbatim, including the
// empty one of a run posted before this plane wrote the field. Deciding what
// an absent identifier means belongs to the caller holding the run it would
// be compared against, not to the reader.
func TestTheListingCarriesEachRunsExternalIdentifier(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	server.mu.Lock()
	server.listPages = []string{`{"total_count":2,"check_runs":[` +
		`{"id":1,"name":"ra8ci-shadow / build","head_sha":"` + reconcilerHead + `","status":"completed",` +
		`"conclusion":"neutral","external_id":"ra8ci-1-abc","output":{"title":"ra8ci observed success"}},` +
		`{"id":2,"name":"ra8ci-shadow / unit-tests","head_sha":"` + reconcilerHead + `","status":"completed",` +
		`"conclusion":"neutral","output":{"title":"ra8ci observed success"}}]}`}
	server.mu.Unlock()
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("published runs: %v", err)
	}
	if len(published.Runs) != 2 {
		t.Fatalf("listed %d runs, want 2", len(published.Runs))
	}
	if published.Runs[0].ExternalID != "ra8ci-1-abc" {
		t.Fatalf("first run carried external id %q", published.Runs[0].ExternalID)
	}
	if published.Runs[1].ExternalID != "" {
		t.Fatalf("a run with no external id carried %q", published.Runs[1].ExternalID)
	}
}

// publishedRunWithSummaryJSON renders one listed run carrying an output body.
// It is a separate helper rather than a wider publishedRunJSON so every test
// above keeps saying exactly what it is about.
func publishedRunWithSummaryJSON(id int64, name, head, status, conclusion, title, summary string) string {
	return fmt.Sprintf(`{"id":%d,"name":%q,"head_sha":%q,"status":%q,"conclusion":%q,"external_id":"","output":{"title":%q,"summary":%q}}`,
		id, name, head, status, conclusion, title, summary)
}

// reconcilerRunName is the check run name one catalog task publishes under.
// reconcilerTaskNames answers with task names, and a listing carrying one of
// those is a listing of runs under no namespace of ours.
func reconcilerRunName(t *testing.T) string {
	t.Helper()
	first, _ := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("check run name: %v", err)
	}
	return name
}

func TestTheListingCarriesEachRunsSummary(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	authoritativeName := reconcilerRunName(t)
	server.serveListing(http.StatusOK, `{"total_count":1,"check_runs":[`+
		publishedRunWithSummaryJSON(4, authoritativeName, reconcilerHead, "completed", "success",
			"ok", "attempt 2 on board ra8d2-07, 41 cases, 0 failures")+`]}`)

	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("published runs: %v", err)
	}
	if len(published.Runs) != 1 {
		t.Fatalf("runs = %d, want 1", len(published.Runs))
	}
	if published.Runs[0].Summary != "attempt 2 on board ra8d2-07, 41 cases, 0 failures" {
		t.Fatalf("summary = %q", published.Runs[0].Summary)
	}
	if published.Runs[0].Title != "ok" {
		t.Fatalf("title = %q, want the title left alone", published.Runs[0].Title)
	}
}

// A run with no output body is reported with an empty summary and not refused:
// the field is somebody else's to fill, and a publisher that wrote only a
// title is a state the listing has to be able to describe.
func TestARunWithNoSummaryIsStillReported(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	authoritativeName := reconcilerRunName(t)
	server.serveListing(http.StatusOK, `{"total_count":1,"check_runs":[`+
		publishedRunJSON(6, authoritativeName, reconcilerHead, "completed", "success", "ok")+`]}`)

	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("published runs: %v", err)
	}
	if len(published.Runs) != 1 || published.Runs[0].Summary != "" {
		t.Fatalf("runs = %+v, want one run with no summary", published.Runs)
	}
}

// Two runs under one name whose summaries differ are both reported with their
// own words. This is the whole reason the field is read: the listing is what
// an operator has to tell them apart from.
func TestTwoRunsUnderOneNameKeepTheirOwnSummaries(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	authoritativeName := reconcilerRunName(t)
	server.serveListing(http.StatusOK, `{"total_count":2,"check_runs":[`+
		publishedRunWithSummaryJSON(11, authoritativeName, reconcilerHead, "completed", "failure",
			"failed", "flash write timed out")+`,`+
		publishedRunWithSummaryJSON(12, authoritativeName, reconcilerHead, "completed", "failure",
			"failed", "board never came back from reset")+`]}`)

	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("published runs: %v", err)
	}
	if len(published.Runs) != 2 {
		t.Fatalf("runs = %d, want 2", len(published.Runs))
	}
	if published.Runs[0].Summary != "flash write timed out" ||
		published.Runs[1].Summary != "board never came back from reset" {
		t.Fatalf("summaries = %q / %q", published.Runs[0].Summary, published.Runs[1].Summary)
	}
}

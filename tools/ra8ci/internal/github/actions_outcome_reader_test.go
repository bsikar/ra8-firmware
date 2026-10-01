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

const actionsRunHead = "0123456789abcdef0123456789abcdef01234567"

type actionsRunServer struct {
	mu          sync.Mutex
	tokenBodies []installationTokenRequest
	methods     []string
	paths       []string
	queries     []string
	runStatus   int
	runBody     string
	jobStatus   int
	jobPages    []string
	jobRequests int
}

func newActionsOutcomeReader(t *testing.T) (*ActionsOutcomeReader, *actionsRunServer) {
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
	state := &actionsRunServer{
		runStatus: http.StatusOK,
		runBody:   completedRunBody(41, 1, actionsRunHead),
		jobStatus: http.StatusOK,
		jobPages:  []string{`{"total_count":0,"jobs":[]}`},
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
		if strings.HasSuffix(r.URL.Path, "/jobs") {
			page := state.jobRequests
			state.jobRequests++
			w.WriteHeader(state.jobStatus)
			if state.jobStatus == http.StatusOK && page < len(state.jobPages) {
				_, _ = w.Write([]byte(state.jobPages[page]))
			}
			return
		}
		w.WriteHeader(state.runStatus)
		if state.runStatus == http.StatusOK {
			_, _ = w.Write([]byte(state.runBody))
		}
	}))
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{inner: transport, host: strings.TrimPrefix(server.URL, "https://")}}
	reader, err := NewActionsOutcomeReader(ActionsOutcomeReaderConfig{
		AppClientID: "Iv1.test", InstallationID: 7, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatalf("new reader: %v", err)
	}
	return reader, state
}

func actionsReaderTask(t *testing.T) string {
	t.Helper()
	names := catalogNames(t)
	if len(names) == 0 {
		t.Fatal("catalog carries no tasks")
	}
	return names[0]
}

func completedRunBody(id int64, attempt int, head string) string {
	return fmt.Sprintf(`{"id":%d,"run_attempt":%d,"head_sha":%q,"status":"completed",
		"repository":{"full_name":"bsikar/ra8-firmware"}}`, id, attempt, head)
}

func (s *actionsRunServer) serveRun(status int, body string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.runStatus, s.runBody = status, body
}

func (s *actionsRunServer) serveJobs(status int, pages ...string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.jobStatus, s.jobPages, s.jobRequests = status, pages, 0
}

func (s *actionsRunServer) seen() ([]string, []string, []string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string{}, s.methods...), append([]string{}, s.paths...), append([]string{}, s.queries...)
}

// A completed run's jobs come back in the order GitHub listed them, each job's
// name, head and conclusion carried through untouched.
func TestOutcomesReportTheRunGitHubExecuted(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	server.serveJobs(http.StatusOK, `{"total_count":3,"jobs":[
		{"id":1,"run_id":41,"name":"build (ubuntu-latest)","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"success"},
		{"id":2,"run_id":41,"name":"lint","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"failure"},
		{"id":3,"run_id":41,"name":"docs","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"skipped"}]}`)
	run, err := reader.Outcomes(context.Background(), 41)
	if err != nil {
		t.Fatalf("outcomes: %v", err)
	}
	if run.RunID != 41 || run.Attempt != 1 || run.HeadSHA != actionsRunHead {
		t.Fatalf("run anchor = %d attempt %d head %s", run.RunID, run.Attempt, run.HeadSHA)
	}
	want := []ActionsOutcome{
		{Job: "build (ubuntu-latest)", HeadSHA: actionsRunHead, Conclusion: "success"},
		{Job: "lint", HeadSHA: actionsRunHead, Conclusion: "failure"},
		{Job: "docs", HeadSHA: actionsRunHead, Conclusion: "skipped"},
	}
	if len(run.Outcomes) != len(want) {
		t.Fatalf("outcomes = %#v", run.Outcomes)
	}
	for i := range want {
		if run.Outcomes[i] != want[i] {
			t.Fatalf("outcome %d = %#v, want %#v", i, run.Outcomes[i], want[i])
		}
	}
}

// The attempt travels with the outcomes and decides which jobs are read. A
// re-run answers the same run ID with different conclusions, and evidence that
// does not name its attempt cannot be checked a second time.
func TestOutcomesReadTheRunsCurrentAttempt(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	server.serveRun(http.StatusOK, completedRunBody(41, 3, actionsRunHead))
	server.serveJobs(http.StatusOK, `{"total_count":1,"jobs":[
		{"id":9,"run_id":41,"name":"lint","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"success"}]}`)
	run, err := reader.Outcomes(context.Background(), 41)
	if err != nil {
		t.Fatalf("outcomes: %v", err)
	}
	if run.Attempt != 3 {
		t.Fatalf("attempt = %d, want 3", run.Attempt)
	}
	_, paths, _ := server.seen()
	wantPath := "/repos/bsikar/ra8-firmware/actions/runs/41/attempts/3/jobs"
	found := false
	for _, path := range paths {
		if path == wantPath {
			found = true
		}
		if strings.Contains(path, "/attempts/1/") || strings.Contains(path, "/attempts/2/") {
			t.Fatalf("read a superseded attempt: %s", path)
		}
	}
	if !found {
		t.Fatalf("never read the current attempt, paths = %v", paths)
	}
}

// Reading the run only reads. Discovering what Actions concluded must not be
// able to change it.
func TestReadingTheRunOnlyReads(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	server.serveJobs(http.StatusOK, `{"total_count":1,"jobs":[
		{"id":1,"run_id":41,"name":"lint","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"success"}]}`)
	if _, err := reader.Outcomes(context.Background(), 41); err != nil {
		t.Fatalf("outcomes: %v", err)
	}
	methods, paths, _ := server.seen()
	for i, method := range methods {
		if strings.HasSuffix(paths[i], "/access_tokens") {
			continue
		}
		if method != http.MethodGet {
			t.Fatalf("request %d to %s used %s", i, paths[i], method)
		}
	}
}

// The token carries actions:read on one repository and nothing else. The
// publisher's checks:write and the gate reader's administration:read are not
// widened to gather evidence.
func TestOutcomeReaderMintsOneNarrowToken(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	if _, err := reader.Outcomes(context.Background(), 41); err != nil {
		t.Fatalf("outcomes: %v", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if len(server.tokenBodies) != 1 {
		t.Fatalf("minted %d tokens", len(server.tokenBodies))
	}
	token := server.tokenBodies[0]
	if len(token.Repositories) != 1 || token.Repositories[0] != "ra8-firmware" {
		t.Fatalf("token repositories = %v", token.Repositories)
	}
	if len(token.Permissions) != 1 || token.Permissions["actions"] != "read" {
		t.Fatalf("token permissions = %v", token.Permissions)
	}
}

// A run GitHub will not return is a failed read, never a run with no jobs. An
// empty job list collects as every covered task indeterminate, which reads as
// a comparison nobody made rather than a read that did not happen.
func TestUnreadableRunIsNeverAnEmptyRun(t *testing.T) {
	for _, status := range []int{http.StatusNotFound, http.StatusForbidden, http.StatusUnauthorized, http.StatusInternalServerError} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			reader, server := newActionsOutcomeReader(t)
			server.serveRun(status, "")
			run, err := reader.Outcomes(context.Background(), 41)
			if !errors.Is(err, ErrActionsRunUnreadable) {
				t.Fatalf("err = %v, want ErrActionsRunUnreadable", err)
			}
			if len(run.Outcomes) != 0 || run.RunID != 0 || run.HeadSHA != "" {
				t.Fatalf("refused read still reported %#v", run)
			}
		})
	}
}

// A jobs page GitHub will not return refuses the whole read. A collection
// assembled from part of a run grades the missing jobs as though they never
// ran.
func TestAPartialJobListingRefusesTheWholeRead(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	server.serveJobs(http.StatusServiceUnavailable)
	run, err := reader.Outcomes(context.Background(), 41)
	if !errors.Is(err, ErrActionsRunUnreadable) {
		t.Fatalf("err = %v, want ErrActionsRunUnreadable", err)
	}
	if len(run.Outcomes) != 0 {
		t.Fatalf("refused read reported outcomes: %#v", run.Outcomes)
	}
}

// A run still executing is refused. Its jobs have conclusions that are simply
// not in yet, and banking them as indeterminate records a comparison a later
// read would have decided.
func TestRunStillExecutingIsRefused(t *testing.T) {
	for _, status := range []string{"in_progress", "queued", "waiting", "requested", "pending"} {
		t.Run(status, func(t *testing.T) {
			reader, server := newActionsOutcomeReader(t)
			server.serveRun(http.StatusOK, fmt.Sprintf(`{"id":41,"run_attempt":1,"head_sha":%q,"status":%q,
				"repository":{"full_name":"bsikar/ra8-firmware"}}`, actionsRunHead, status))
			if _, err := reader.Outcomes(context.Background(), 41); !errors.Is(err, ErrActionsRunIncomplete) {
				t.Fatalf("err = %v, want ErrActionsRunIncomplete", err)
			}
		})
	}
}

// A run that does not describe itself as the one asked for is refused, so a
// redirected or confused answer cannot be graded as this commit's evidence.
func TestRunMustDescribeItself(t *testing.T) {
	cases := map[string]string{
		"another run":        completedRunBody(99, 1, actionsRunHead),
		"another repository": `{"id":41,"run_attempt":1,"head_sha":"` + actionsRunHead + `","status":"completed","repository":{"full_name":"someone/else"}}`,
		"no attempt":         completedRunBody(41, 0, actionsRunHead),
		"short head":         completedRunBody(41, 1, "0123456789"),
		"not an object":      `["run"]`,
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			reader, server := newActionsOutcomeReader(t)
			server.serveRun(http.StatusOK, body)
			if _, err := reader.Outcomes(context.Background(), 41); !errors.Is(err, ErrActionsRunUnreadable) {
				t.Fatalf("err = %v, want ErrActionsRunUnreadable", err)
			}
		})
	}
}

// A job listed under another run is refused rather than carried: it is
// evidence about a commit this read is not reporting on.
func TestAJobFromAnotherRunIsRefused(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	server.serveJobs(http.StatusOK, `{"total_count":1,"jobs":[
		{"id":5,"run_id":77,"name":"lint","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"success"}]}`)
	if _, err := reader.Outcomes(context.Background(), 41); !errors.Is(err, ErrActionsRunUnreadable) {
		t.Fatalf("err = %v, want ErrActionsRunUnreadable", err)
	}
}

// Names and conclusions are reported verbatim for Collect to judge, the same
// division RequiredCheckReader draws with the gate: one place decides what a
// usable pairing is, and it is the one that grades.
func TestOutcomesAreReportedForCollectToJudge(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	server.serveJobs(http.StatusOK, `{"total_count":2,"jobs":[
		{"id":1,"run_id":41,"name":"lint","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"success"},
		{"id":2,"run_id":41,"name":"lint","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"failure"}]}`)
	run, err := reader.Outcomes(context.Background(), 41)
	if err != nil {
		t.Fatalf("outcomes: %v", err)
	}
	if len(run.Outcomes) != 2 {
		t.Fatalf("reader tidied the run: %#v", run.Outcomes)
	}
	correspondence, err := NewShadowCorrespondence(map[string]string{actionsReaderTask(t): "lint"}, catalogNames(t))
	if err != nil {
		t.Fatalf("correspondence: %v", err)
	}
	_, err = correspondence.Collect([]PlaneOutcome{{Task: actionsReaderTask(t), HeadSHA: actionsRunHead, Observed: "success"}}, run.Outcomes)
	if !errors.Is(err, ErrShadowSetAmbiguous) {
		t.Fatalf("collect err = %v, want ErrShadowSetAmbiguous", err)
	}
}

// A job of a completed run that never concluded is carried with an empty
// conclusion, which CompareShadowRun grades as indeterminate: the true answer
// for a pairing nobody judged, and distinguishable from a task never run.
func TestAJobWithoutAConclusionIsCarriedUnjudged(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	server.serveJobs(http.StatusOK, `{"total_count":1,"jobs":[
		{"id":1,"run_id":41,"name":"lint","head_sha":"`+actionsRunHead+`","status":"queued","conclusion":""}]}`)
	run, err := reader.Outcomes(context.Background(), 41)
	if err != nil {
		t.Fatalf("outcomes: %v", err)
	}
	if len(run.Outcomes) != 1 || run.Outcomes[0].Conclusion != "" {
		t.Fatalf("outcomes = %#v", run.Outcomes)
	}
	correspondence, err := NewShadowCorrespondence(map[string]string{actionsReaderTask(t): "lint"}, catalogNames(t))
	if err != nil {
		t.Fatalf("correspondence: %v", err)
	}
	collection, err := correspondence.Collect(
		[]PlaneOutcome{{Task: actionsReaderTask(t), HeadSHA: actionsRunHead, Observed: "success"}}, run.Outcomes)
	if err != nil {
		t.Fatalf("collect: %v", err)
	}
	report, err := CompareShadowRun(collection.Observations)
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	if report.Indeterminate != 1 || report.Clean() {
		t.Fatalf("report = %#v", report)
	}
}

// Paging stops at the total GitHub reported, and every page is read before any
// outcome is returned.
func TestOutcomesPageThroughTheWholeRun(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	first := make([]string, 0, 100)
	for i := 0; i < 100; i++ {
		first = append(first, fmt.Sprintf(`{"id":%d,"run_id":41,"name":"job-%03d","head_sha":%q,"status":"completed","conclusion":"success"}`,
			i+1, i, actionsRunHead))
	}
	server.serveJobs(http.StatusOK,
		`{"total_count":101,"jobs":[`+strings.Join(first, ",")+`]}`,
		`{"total_count":101,"jobs":[{"id":101,"run_id":41,"name":"last","head_sha":"`+actionsRunHead+`","status":"completed","conclusion":"failure"}]}`)
	run, err := reader.Outcomes(context.Background(), 41)
	if err != nil {
		t.Fatalf("outcomes: %v", err)
	}
	if len(run.Outcomes) != 101 || run.Outcomes[100].Job != "last" {
		t.Fatalf("collected %d outcomes, last %#v", len(run.Outcomes), run.Outcomes[len(run.Outcomes)-1])
	}
	_, _, queries := server.seen()
	if !strings.Contains(strings.Join(queries, " "), "page=2") {
		t.Fatalf("never asked for the second page: %v", queries)
	}
}

// A run larger than this reader pages through is refused rather than reported
// short, for the same reason a partial listing is.
func TestARunTooLargeToCollectIsRefused(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	page := make([]string, 0, 100)
	for i := 0; i < 100; i++ {
		page = append(page, fmt.Sprintf(`{"id":%d,"run_id":41,"name":"job-%03d","head_sha":%q,"status":"completed","conclusion":"success"}`,
			i+1, i, actionsRunHead))
	}
	pages := make([]string, 0, maxActionsJobPages)
	for i := 0; i < maxActionsJobPages; i++ {
		pages = append(pages, `{"total_count":5000,"jobs":[`+strings.Join(page, ",")+`]}`)
	}
	server.serveJobs(http.StatusOK, pages...)
	if _, err := reader.Outcomes(context.Background(), 41); !errors.Is(err, ErrActionsRunTooLarge) {
		t.Fatalf("err = %v, want ErrActionsRunTooLarge", err)
	}
}

// A run number that cannot name a run is refused before a token is minted.
func TestAnImpossibleRunNumberIsRefusedBeforeMintingAToken(t *testing.T) {
	for _, runID := range []int64{0, -1} {
		reader, server := newActionsOutcomeReader(t)
		if _, err := reader.Outcomes(context.Background(), runID); err == nil {
			t.Fatalf("run %d was accepted", runID)
		}
		server.mu.Lock()
		minted := len(server.tokenBodies)
		server.mu.Unlock()
		if minted != 0 {
			t.Fatalf("run %d minted %d tokens", runID, minted)
		}
	}
}

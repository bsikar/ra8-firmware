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

const commitRunsHead = "abcdef0123456789abcdef0123456789abcdef01"

type commitRunsServer struct {
	mu           sync.Mutex
	tokenBodies  []installationTokenRequest
	methods      []string
	paths        []string
	queries      []string
	listStatus   int
	listPages    []string
	listRequests int
	runStatus    int
	runBody      string
	jobsBody     string
}

func newCommitRunsReader(t *testing.T) (*ActionsOutcomeReader, *commitRunsServer) {
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
	state := &commitRunsServer{
		listStatus: http.StatusOK,
		listPages:  []string{`{"total_count":0,"workflow_runs":[]}`},
		runStatus:  http.StatusOK,
		jobsBody:   `{"total_count":0,"jobs":[]}`,
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		state.mu.Lock()
		defer state.mu.Unlock()
		state.methods = append(state.methods, r.Method)
		state.paths = append(state.paths, r.URL.Path)
		state.queries = append(state.queries, r.URL.RawQuery)
		switch {
		case strings.HasSuffix(r.URL.Path, "/access_tokens"):
			var body installationTokenRequest
			_ = json.NewDecoder(r.Body).Decode(&body)
			state.tokenBodies = append(state.tokenBodies, body)
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour)})
		case strings.HasSuffix(r.URL.Path, "/jobs"):
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte(state.jobsBody))
		case strings.HasSuffix(r.URL.Path, "/actions/runs"):
			page := state.listRequests
			state.listRequests++
			w.WriteHeader(state.listStatus)
			if state.listStatus == http.StatusOK && page < len(state.listPages) {
				_, _ = w.Write([]byte(state.listPages[page]))
			}
		default:
			w.WriteHeader(state.runStatus)
			if state.runStatus == http.StatusOK {
				_, _ = w.Write([]byte(state.runBody))
			}
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

func (s *commitRunsServer) serveList(status int, pages ...string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.listStatus, s.listPages, s.listRequests = status, pages, 0
}

func (s *commitRunsServer) serveRunAndJobs(run, jobs string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.runBody, s.jobsBody = run, jobs
}

func (s *commitRunsServer) seenRequests() ([]string, []string, []string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string{}, s.methods...), append([]string{}, s.paths...), append([]string{}, s.queries...)
}

func listedRun(id int64, name, status, conclusion string) string {
	return fmt.Sprintf(`{"id":%d,"name":%q,"run_attempt":1,"head_sha":%q,"event":"pull_request",
		"status":%q,"conclusion":%q,"repository":{"full_name":"bsikar/ra8-firmware"}}`,
		id, name, commitRunsHead, status, conclusion)
}

// The runs come back in the order GitHub listed them, each run's workflow
// name, attempt, event, status and conclusion carried through untouched.
func TestTheCommitsRunsAreReportedVerbatim(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK, `{"total_count":2,"workflow_runs":[`+
		listedRun(41, "checks", "completed", "failure")+`,`+
		listedRun(40, "docs", "completed", "success")+`]}`)
	runs, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("runs on: %v", err)
	}
	if runs.HeadSHA != commitRunsHead || len(runs.Runs) != 2 {
		t.Fatalf("runs = %#v", runs)
	}
	want := []CommitWorkflowRun{
		{ID: 41, Workflow: "checks", Attempt: 1, Event: "pull_request", Status: "completed", Conclusion: "failure"},
		{ID: 40, Workflow: "docs", Attempt: 1, Event: "pull_request", Status: "completed", Conclusion: "success"},
	}
	for i := range want {
		if runs.Runs[i] != want[i] {
			t.Fatalf("run %d = %#v, want %#v", i, runs.Runs[i], want[i])
		}
	}
}

// The listing asks GitHub for this commit and only reads. Discovering which
// runs a commit carries must not be able to change any of them.
func TestTheListingAsksForTheCommitAndOnlyReads(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK, `{"total_count":1,"workflow_runs":[`+listedRun(41, "checks", "completed", "success")+`]}`)
	if _, err := reader.RunsOn(context.Background(), commitRunsHead); err != nil {
		t.Fatalf("runs on: %v", err)
	}
	methods, paths, queries := server.seenRequests()
	asked := false
	for i, path := range paths {
		if strings.HasSuffix(path, "/access_tokens") {
			continue
		}
		if methods[i] != http.MethodGet {
			t.Fatalf("request %d to %s used %s", i, path, methods[i])
		}
		if path == "/repos/bsikar/ra8-firmware/actions/runs" && strings.Contains(queries[i], "head_sha="+commitRunsHead) {
			asked = true
		}
	}
	if !asked {
		t.Fatalf("never asked for the commit's runs, paths = %v queries = %v", paths, queries)
	}
}

// A listing GitHub will not return is a failed read, never a commit with no
// runs. An empty listing reads as a commit CI never touched, which is exactly
// the state an operator choosing representative pull requests has to be able
// to tell apart from a read that did not happen.
func TestAnUnreadableListingIsNeverACommitWithNoRuns(t *testing.T) {
	for _, status := range []int{http.StatusNotFound, http.StatusForbidden, http.StatusUnauthorized, http.StatusInternalServerError} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			reader, server := newCommitRunsReader(t)
			server.serveList(status, "")
			runs, err := reader.RunsOn(context.Background(), commitRunsHead)
			if !errors.Is(err, ErrCommitRunsUnreadable) {
				t.Fatalf("err = %v, want ErrCommitRunsUnreadable", err)
			}
			if len(runs.Runs) != 0 || runs.HeadSHA != "" {
				t.Fatalf("refused read still reported %#v", runs)
			}
		})
	}
}

// A run still executing is listed, not refused. Outcomes refuses to grade one
// because an unfinished run banks pairings a later read would decide; hiding
// it from the listing would tell an operator CI is finished with a commit it
// has not started on. Completed() is what selects the gradable runs.
func TestARunStillExecutingIsListedAndSetApart(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK, `{"total_count":3,"workflow_runs":[`+
		listedRun(43, "checks", "in_progress", "")+`,`+
		listedRun(42, "nightly", "queued", "")+`,`+
		listedRun(41, "docs", "completed", "success")+`]}`)
	runs, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("runs on: %v", err)
	}
	if len(runs.Runs) != 3 {
		t.Fatalf("listing dropped a run: %#v", runs.Runs)
	}
	if runs.Runs[0].Conclusion != "" || runs.Runs[0].Status != "in_progress" {
		t.Fatalf("unfinished run = %#v", runs.Runs[0])
	}
	completed := runs.Completed()
	if len(completed) != 1 || completed[0].ID != 41 {
		t.Fatalf("completed = %#v", completed)
	}
}

// A run listed against another commit or another repository refuses the whole
// listing, for the reason a job listed under another run does: it is evidence
// about a commit this read does not report on.
func TestARunFromSomewhereElseRefusesTheListing(t *testing.T) {
	cases := map[string]string{
		"another commit": `{"id":41,"name":"checks","run_attempt":1,"head_sha":"1111111111111111111111111111111111111111",
			"status":"completed","conclusion":"success","repository":{"full_name":"bsikar/ra8-firmware"}}`,
		"another repository": `{"id":41,"name":"checks","run_attempt":1,"head_sha":"` + commitRunsHead + `",
			"status":"completed","conclusion":"success","repository":{"full_name":"someone/else"}}`,
		"no attempt": `{"id":41,"name":"checks","run_attempt":0,"head_sha":"` + commitRunsHead + `",
			"status":"completed","conclusion":"success","repository":{"full_name":"bsikar/ra8-firmware"}}`,
		"no run": `{"id":0,"name":"checks","run_attempt":1,"head_sha":"` + commitRunsHead + `",
			"status":"completed","conclusion":"success","repository":{"full_name":"bsikar/ra8-firmware"}}`,
		"not an object": `["runs"]`,
	}
	for name, run := range cases {
		t.Run(name, func(t *testing.T) {
			reader, server := newCommitRunsReader(t)
			body := `{"total_count":1,"workflow_runs":[` + run + `]}`
			if name == "not an object" {
				body = run
			}
			server.serveList(http.StatusOK, body)
			runs, err := reader.RunsOn(context.Background(), commitRunsHead)
			if !errors.Is(err, ErrCommitRunsUnreadable) {
				t.Fatalf("err = %v, want ErrCommitRunsUnreadable", err)
			}
			if len(runs.Runs) != 0 {
				t.Fatalf("refused listing still reported %#v", runs.Runs)
			}
		})
	}
}

// GitHub renders a commit in either case, and the same commit written
// differently is not a different commit.
func TestGitHubsCasingOfTheCommitIsNotADifferentCommit(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	upper := strings.ToUpper(commitRunsHead)
	server.serveList(http.StatusOK, fmt.Sprintf(`{"total_count":1,"workflow_runs":[
		{"id":41,"name":"checks","run_attempt":1,"head_sha":%q,"status":"completed","conclusion":"success",
		 "repository":{"full_name":"bsikar/ra8-firmware"}}]}`, upper))
	runs, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("runs on: %v", err)
	}
	if len(runs.Runs) != 1 || runs.Runs[0].ID != 41 {
		t.Fatalf("runs = %#v", runs.Runs)
	}
}

// Listing a commit's runs is actions:read, the permission this reader already
// holds. The pull-request reader's pull_requests:read is not widened to save a
// hop, and no second permission joins this token.
func TestListingSpendsTheActionsTokenAlone(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK, `{"total_count":1,"workflow_runs":[`+listedRun(41, "checks", "completed", "success")+`]}`)
	if _, err := reader.RunsOn(context.Background(), commitRunsHead); err != nil {
		t.Fatalf("runs on: %v", err)
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

// Something that is not a commit is refused before a token is minted, so a
// mistyped head never reaches GitHub as a request at all.
func TestANonCommitIsRefusedBeforeATokenIsMinted(t *testing.T) {
	for _, head := range []string{"", "0123456789", "ra8ci/dev", commitRunsHead + "0", " " + commitRunsHead} {
		t.Run(fmt.Sprintf("%q", head), func(t *testing.T) {
			reader, server := newCommitRunsReader(t)
			if _, err := reader.RunsOn(context.Background(), head); !errors.Is(err, ErrInvalidCheckRunSHA) {
				t.Fatalf("err = %v, want ErrInvalidCheckRunSHA", err)
			}
			methods, _, _ := server.seenRequests()
			if len(methods) != 0 {
				t.Fatalf("refused head still reached GitHub: %v", methods)
			}
		})
	}
}

// Paging stops at the total GitHub reported, and every page is read before any
// run is returned.
func TestTheListingPagesThroughTheWholeCommit(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	first := make([]string, 0, 100)
	for i := 0; i < 100; i++ {
		first = append(first, listedRun(int64(i+1), fmt.Sprintf("workflow-%03d", i), "completed", "success"))
	}
	server.serveList(http.StatusOK,
		`{"total_count":101,"workflow_runs":[`+strings.Join(first, ",")+`]}`,
		`{"total_count":101,"workflow_runs":[`+listedRun(101, "last", "completed", "failure")+`]}`)
	runs, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("runs on: %v", err)
	}
	if len(runs.Runs) != 101 || runs.Runs[100].Workflow != "last" {
		t.Fatalf("listed %d runs, last %#v", len(runs.Runs), runs.Runs[len(runs.Runs)-1])
	}
	_, _, queries := server.seenRequests()
	if !strings.Contains(strings.Join(queries, " "), "page=2") {
		t.Fatalf("never asked for the second page: %v", queries)
	}
}

// The listing hands Outcomes a run ID it can grade: the two halves of the
// Actions reader meet at a run this plane found rather than one a person typed
// out of the web interface.
func TestACommitsRunFeedsTheOutcomeReader(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK, `{"total_count":2,"workflow_runs":[`+
		listedRun(43, "nightly", "queued", "")+`,`+
		listedRun(41, "checks", "completed", "failure")+`]}`)
	server.serveRunAndJobs(
		fmt.Sprintf(`{"id":41,"run_attempt":1,"head_sha":%q,"status":"completed","repository":{"full_name":"bsikar/ra8-firmware"}}`, commitRunsHead),
		fmt.Sprintf(`{"total_count":1,"jobs":[{"id":9,"run_id":41,"name":"lint","head_sha":%q,"status":"completed","conclusion":"failure"}]}`, commitRunsHead))
	runs, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("runs on: %v", err)
	}
	completed := runs.Completed()
	if len(completed) != 1 {
		t.Fatalf("completed = %#v", completed)
	}
	outcomes, err := reader.Outcomes(context.Background(), completed[0].ID)
	if err != nil {
		t.Fatalf("outcomes: %v", err)
	}
	if outcomes.RunID != 41 || outcomes.HeadSHA != commitRunsHead ||
		len(outcomes.Outcomes) != 1 || outcomes.Outcomes[0].Conclusion != "failure" {
		t.Fatalf("outcomes = %#v", outcomes)
	}
}

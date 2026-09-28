// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Resolved metadata is what binds a queued scale-set job to one workflow
// attempt and one commit, and everything downstream trusts it. So a job
// this resolver cannot account for exactly must be refused, never guessed
// at. metadata_test.go pins the happy path, the branch mismatch and the
// path traversal; this pins the request it will not make and the answers
// it will not believe.

const resolvedSHA = "9f2c1b7ad4e6f8091a2b3c4d5e6f708192a3b4c5"

// forge stands a fake api.github.com up, mints the App key, and hands back
// a resolver pointed at it along with a count of requests per path.
func forge(t *testing.T, jobs http.HandlerFunc, run *workflowRunResponse) (*MetadataResolver, map[string]int) {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	if err := os.WriteFile(keyPath, keyPEM, 0o600); err != nil {
		t.Fatal(err)
	}

	asked := map[string]int{}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.URL.Path == "/app/installations/42/access_tokens":
			asked["token"]++
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{
				Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour),
			})
		case r.URL.Path == "/repos/bsikar/ra8-firmware/actions/runs/1234":
			asked["run"]++
			if run == nil {
				http.Error(w, "nope", http.StatusNotFound)
				return
			}
			_ = json.NewEncoder(w).Encode(run)
		case strings.HasSuffix(r.URL.Path, "/jobs"):
			asked["jobs"]++
			jobs(w, r)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(server.Close)

	destination, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	client := server.Client()
	client.Transport = metadataTestTransport{destination: destination, transport: client.Transport}

	resolver, err := NewMetadataResolver(MetadataConfig{
		APIBaseURL: "https://api.github.com", AppClientID: "client-id", InstallationID: 42,
		PrivateKeyFile: keyPath, Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	return resolver, asked
}

// soundRun is the workflow run the fixture job belongs to.
func soundRun() *workflowRunResponse {
	run := &workflowRunResponse{
		ID: 1234, RunAttempt: 2, HeadSHA: resolvedSHA, HeadBranch: "dev",
		Path: ".github/workflows/ci.yml@dev", Event: "push",
	}
	run.Repository.FullName = "bsikar/ra8-firmware"
	return run
}

// queued is the scale-set job the run is checked against.
func queued() Job {
	return Job{
		Owner: "bsikar", Repository: "ra8-firmware", JobID: "job-guid", WorkflowRunID: 1234,
		WorkflowRef: "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev",
		DisplayName: "build", EventName: "push",
	}
}

// oneMatchingJob answers the attempt's job list with the job itself.
func oneMatchingJob(w http.ResponseWriter, _ *http.Request) {
	_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 1, "jobs": []map[string]any{{
		"id": 91, "run_id": 1234, "name": "build", "head_sha": resolvedSHA, "head_branch": "dev",
	}}})
}

// A request this resolver has no business making is refused before a token
// is minted: nothing reaches GitHub on behalf of another repository.
func TestAForeignOrIncompleteRequestNeverReachesGitHub(t *testing.T) {
	sound := queued()

	otherOwner := sound
	otherOwner.Owner = "someone-else"

	otherRepository := sound
	otherRepository.Repository = "another-repo"

	noRun := sound
	noRun.WorkflowRunID = 0

	negativeRun := sound
	negativeRun.WorkflowRunID = -1

	noJobID := sound
	noJobID.JobID = ""

	longJobID := sound
	longJobID.JobID = strings.Repeat("j", 257)

	noDisplayName := sound
	noDisplayName.DisplayName = ""

	noEvent := sound
	noEvent.EventName = ""

	resolver, asked := forge(t, oneMatchingJob, soundRun())
	for name, job := range map[string]Job{
		"another owner":      otherOwner,
		"another repository": otherRepository,
		"no run":             noRun,
		"a negative run":     negativeRun,
		"no job id":          noJobID,
		"an over-long id":    longJobID,
		"no display name":    noDisplayName,
		"no event":           noEvent,
	} {
		if _, err := resolver.Resolve(context.Background(), job); err == nil ||
			!strings.Contains(err.Error(), "invalid or foreign") {
			t.Fatalf("%s was not refused: %v", name, err)
		}
	}
	if _, err := resolver.Resolve(nil, sound); err == nil {
		t.Fatal("a nil caller was accepted")
	}
	if asked["token"] != 0 || asked["run"] != 0 {
		t.Fatalf("GitHub was asked: %v", asked)
	}

	// A workflow reference that is not an exact branch ref is refused on
	// the same terms, since a tag or a SHA ref would not pin a branch.
	for _, ref := range []string{
		"", "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/tags/v1",
		"bsikar/ra8-firmware/.github/workflows/ci.yml", "someone-else/repo/.github/workflows/ci.yml@refs/heads/dev",
	} {
		job := sound
		job.WorkflowRef = ref
		if _, err := resolver.Resolve(context.Background(), job); err == nil ||
			!strings.Contains(err.Error(), "branch ref") {
			t.Fatalf("ref %q was accepted: %v", ref, err)
		}
	}
	if asked["token"] != 0 {
		t.Fatalf("a token was minted for a reference that was never going to be used: %v", asked)
	}
}

// The run GitHub answers with must be the run that was asked about, in
// every field the job claims. A mismatch means the queued job and the
// workflow attempt are not the same thing, so nothing is resolved.
func TestARunThatDoesNotMatchTheQueuedJobResolvesNothing(t *testing.T) {
	wrongID := soundRun()
	wrongID.ID = 5678

	noAttempt := soundRun()
	noAttempt.RunAttempt = 0

	otherRepository := soundRun()
	otherRepository.Repository.FullName = "someone-else/ra8-firmware"

	otherEvent := soundRun()
	otherEvent.Event = "pull_request"

	otherBranch := soundRun()
	otherBranch.HeadBranch = "main"

	otherWorkflow := soundRun()
	otherWorkflow.Path = ".github/workflows/release.yml@dev"

	shortSHA := soundRun()
	shortSHA.HeadSHA = "9f2c1b7"

	unhexSHA := soundRun()
	unhexSHA.HeadSHA = strings.Repeat("z", 40)

	for name, run := range map[string]*workflowRunResponse{
		"another run":        wrongID,
		"no attempt":         noAttempt,
		"another repository": otherRepository,
		"another event":      otherEvent,
		"another branch":     otherBranch,
		"another workflow":   otherWorkflow,
		"a short SHA":        shortSHA,
		"a non-hex SHA":      unhexSHA,
	} {
		resolver, _ := forge(t, oneMatchingJob, run)
		if _, err := resolver.Resolve(context.Background(), queued()); err == nil ||
			!strings.Contains(err.Error(), "does not match") {
			t.Fatalf("%s was accepted: %v", name, err)
		}
	}
}

// An answer the resolver cannot read is not an answer. None of these may
// resolve to metadata, since the whole point is that it is trusted.
func TestAnAnswerTheResolverCannotReadIsRefused(t *testing.T) {
	missing, _ := forge(t, oneMatchingJob, nil)
	if _, err := missing.Resolve(context.Background(), queued()); err == nil ||
		!strings.Contains(err.Error(), "HTTP 404") {
		t.Fatalf("a missing run: %v", err)
	}

	// A body that is not JSON at all, served with a 200.
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	if err := os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{
		Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key),
	}), 0o600); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "access_tokens") {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{
				Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour),
			})
			return
		}
		_, _ = w.Write([]byte("<html>not json</html>"))
	}))
	defer server.Close()
	destination, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	client := server.Client()
	client.Transport = metadataTestTransport{destination: destination, transport: client.Transport}
	resolver, err := NewMetadataResolver(MetadataConfig{
		APIBaseURL: "https://api.github.com", AppClientID: "client-id", InstallationID: 42,
		PrivateKeyFile: keyPath, Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := resolver.Resolve(context.Background(), queued()); err == nil ||
		!strings.Contains(err.Error(), "invalid") {
		t.Fatalf("a non-JSON body: %v", err)
	}
}

// The attempt's job list is read page by page, and the job is looked for
// on each one. A list this plane cannot bound is refused rather than
// scanned, since it decides whether a queued job is real.
func TestTheAttemptJobListIsPagedAndBounded(t *testing.T) {
	// The job sits on the second page, so a resolver that stops after the
	// first would wrongly report the job absent.
	paged := func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Query().Get("page") == "1" {
			filler := make([]map[string]any, 0, 100)
			for i := 0; i < 100; i++ {
				filler = append(filler, map[string]any{
					"id": 500 + i, "run_id": 1234, "name": fmt.Sprintf("other-%d", i),
					"head_sha": resolvedSHA, "head_branch": "dev",
				})
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 150, "jobs": filler})
			return
		}
		oneMatchingJob(w, r)
	}
	resolver, asked := forge(t, paged, soundRun())
	got, err := resolver.Resolve(context.Background(), queued())
	if err != nil {
		t.Fatalf("a job on the second page was not found: %v", err)
	}
	if got.WorkflowAttempt != 2 || got.CommitSHA != resolvedSHA || got.WorkflowRunID != 1234 {
		t.Fatalf("metadata = %+v", got)
	}
	if asked["jobs"] != 2 {
		t.Fatalf("pages read = %d", asked["jobs"])
	}

	// An empty first page ends the search: the attempt holds no jobs.
	empty, asked := forge(t, func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 0, "jobs": []map[string]any{}})
	}, soundRun())
	if _, err := empty.Resolve(context.Background(), queued()); err == nil ||
		!strings.Contains(err.Error(), "does not contain") {
		t.Fatalf("an empty attempt: %v", err)
	}
	if asked["jobs"] != 1 {
		t.Fatalf("an empty page was paged past: %d", asked["jobs"])
	}

	for name, answer := range map[string]http.HandlerFunc{
		"refused": func(w http.ResponseWriter, _ *http.Request) {
			http.Error(w, "forbidden", http.StatusForbidden)
		},
		"unreadable": func(w http.ResponseWriter, _ *http.Request) {
			_, _ = w.Write([]byte("{"))
		},
		"a negative count": func(w http.ResponseWriter, _ *http.Request) {
			_ = json.NewEncoder(w).Encode(map[string]any{"total_count": -1, "jobs": []map[string]any{}})
		},
		"more than a thousand": func(w http.ResponseWriter, _ *http.Request) {
			_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 1001, "jobs": []map[string]any{}})
		},
	} {
		resolver, _ := forge(t, answer, soundRun())
		if _, err := resolver.Resolve(context.Background(), queued()); err == nil {
			t.Fatalf("%s was accepted", name)
		}
	}
}

// Ten full pages of a hundred is exactly the supported attempt, and the
// two bounds meet there: a list claiming more than a thousand jobs is
// refused as out of bounds, so the ten-page loop can never be asked for an
// eleventh page. A full thousand is read, and a job absent from all of it
// is reported absent rather than guessed at.
func TestAFullThousandJobsIsReadAndNoMoreIsAsked(t *testing.T) {
	crowded := func(w http.ResponseWriter, _ *http.Request) {
		filler := make([]map[string]any, 0, 100)
		for i := 0; i < 100; i++ {
			filler = append(filler, map[string]any{
				"id": 600 + i, "run_id": 1234, "name": fmt.Sprintf("other-%d", i),
				"head_sha": resolvedSHA, "head_branch": "dev",
			})
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 1000, "jobs": filler})
	}

	resolver, asked := forge(t, crowded, soundRun())
	_, err := resolver.Resolve(context.Background(), queued())
	if err == nil || !strings.Contains(err.Error(), "does not contain") {
		t.Fatalf("err = %v", err)
	}
	if asked["jobs"] != 10 {
		t.Fatalf("pages read = %d, want the whole thousand across ten pages", asked["jobs"])
	}

	// One job past the thousand is refused as out of bounds on the first
	// page, which is what makes an eleventh page unreachable.
	overfull, asked := forge(t, func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 1001, "jobs": []map[string]any{}})
	}, soundRun())
	if _, err := overfull.Resolve(context.Background(), queued()); err == nil ||
		!strings.Contains(err.Error(), "exceeds supported bounds") {
		t.Fatalf("an over-full attempt: %v", err)
	}
	if asked["jobs"] != 1 {
		t.Fatalf("an out-of-bounds list was paged: %d", asked["jobs"])
	}
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
)

// The jobs pages are the second half of a resolution: the run is fetched,
// then the attempt's jobs are read to confirm the scale-set job is actually
// in it. A page that could not be read must not read as an attempt that does
// not contain the job, because that answer is how a genuinely foreign job is
// refused and it would have a sound job refused for the endpoint's fault.

// soundRunFor answers the run fetch for the job under resolution, and hands
// the attempt's jobs pages to the caller's handler.
func soundRunFor(jobs http.HandlerFunc) http.HandlerFunc {
	run := workflowRunResponse{
		ID: 1234, RunAttempt: 2, HeadSHA: strings.Repeat("a", 40),
		HeadBranch: "dev", Path: ".github/workflows/ci.yml@dev", Event: "push",
	}
	run.Repository.FullName = "bsikar/ra8-firmware"
	return mintedThen(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/jobs") {
			jobs(w, r)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(run)
	})
}

// A jobs page that could not be read is reported as the read it was, never as
// an attempt that does not contain the job.
func TestAJobsPageThatCouldNotBeReadIsNotAMissingJob(t *testing.T) {
	for _, refusal := range []struct {
		name    string
		names   string
		handler http.HandlerFunc
	}{
		{"a connection dropped mid-page", "fetch GitHub workflow jobs", hangUp},
		{"a page the endpoint would not serve", "GitHub workflow jobs returned HTTP 500", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusInternalServerError)
		}},
		{"a page past the response bound", "unreadable or too large", func(w http.ResponseWriter, _ *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(`{"padding":"` + strings.Repeat("p", maxMetadataResponse) + `"}`))
		}},
		{"a page shorter than it promised", "unreadable or too large", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("Content-Length", "8192")
			_, _ = w.Write([]byte(`{"total_count":1,`))
			hangUp(w, r)
		}},
		{"a page that is not a jobs document", "invalid or exceeds supported bounds", func(w http.ResponseWriter, _ *http.Request) {
			_, _ = w.Write([]byte("<html>maintenance</html>"))
		}},
		{"a count past the supported bound", "invalid or exceeds supported bounds", func(w http.ResponseWriter, _ *http.Request) {
			_, _ = w.Write([]byte(`{"total_count":1001,"jobs":[]}`))
		}},
		{"a negative count", "invalid or exceeds supported bounds", func(w http.ResponseWriter, _ *http.Request) {
			_, _ = w.Write([]byte(`{"total_count":-1,"jobs":[]}`))
		}},
	} {
		resolver := resolvedAgainst(t, soundRunFor(refusal.handler))
		metadata, err := resolver.Resolve(context.Background(), scaleSetJobToResolve())
		if err == nil || !strings.Contains(err.Error(), refusal.names) {
			t.Errorf("%s answered %v, want an error naming %q", refusal.name, err, refusal.names)
			continue
		}
		if strings.Contains(err.Error(), "does not contain the scale-set job") {
			t.Errorf("%s was reported as a job that is not in the attempt", refusal.name)
		}
		if metadata.WorkflowAttempt != 0 || metadata.CommitSHA != "" {
			t.Errorf("%s answered %+v", refusal.name, metadata)
		}
	}
}

// A page bearing exactly the bound's worth of jobs document is read, and a
// sound attempt containing the job resolves: the bound refuses size, not
// length of work.
func TestAnAttemptContainingTheJobStillResolvesBesideTheseRefusals(t *testing.T) {
	resolver := resolvedAgainst(t, soundRunFor(func(w http.ResponseWriter, r *http.Request) {
		if page := r.URL.Query().Get("page"); page != "1" {
			t.Errorf("a first-page match asked for page %q as well", page)
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 1, "jobs": []map[string]any{{
			"id": 91, "run_id": 1234, "name": "build",
			"head_sha": strings.Repeat("A", 40), "head_branch": "dev",
		}}})
	}))

	metadata, err := resolver.Resolve(context.Background(), scaleSetJobToResolve())
	if err != nil {
		t.Fatalf("a sound attempt answered %v", err)
	}
	if metadata.WorkflowAttempt != 2 || metadata.WorkflowRunID != 1234 || metadata.Repository != "bsikar/ra8-firmware" {
		t.Fatalf("a sound attempt answered %+v", metadata)
	}
	// The commit travels lowered however the forge spelled it, so two
	// reads of one attempt cannot disagree on the commit they name.
	if metadata.CommitSHA != strings.Repeat("a", 40) {
		t.Fatalf("the commit was carried as %q", metadata.CommitSHA)
	}
}

// An attempt whose pages are sound and simply do not hold the job is the one
// case that IS a missing job, and it keeps its own wording.
func TestAnAttemptWithoutTheJobIsNamedAsSuch(t *testing.T) {
	resolver := resolvedAgainst(t, soundRunFor(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 1, "jobs": []map[string]any{{
			"id": 91, "run_id": 1234, "name": "lint",
			"head_sha": strings.Repeat("a", 40), "head_branch": "dev",
		}}})
	}))

	if _, err := resolver.Resolve(context.Background(), scaleSetJobToResolve()); err == nil ||
		!strings.Contains(err.Error(), "does not contain the scale-set job") {
		t.Fatalf("an attempt without the job answered %v", err)
	}
}

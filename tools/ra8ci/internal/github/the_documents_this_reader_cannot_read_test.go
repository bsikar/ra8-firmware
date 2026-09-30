// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"testing"
)

// A 200 is not an answer. GitHub answering the jobs endpoint with something
// this reader cannot read has to refuse the whole run, for the same reason a
// non-200 does: a collection assembled from part of a run grades the missing
// jobs as though they never ran, and the evidence #1481 holds the gate against
// would be built on a read that failed quietly.
func TestAJobsDocumentThisReaderCannotReadRefusesTheRun(t *testing.T) {
	hundredAndOne := make([]string, 0, 101)
	for i := 0; i < 101; i++ {
		hundredAndOne = append(hundredAndOne, fmt.Sprintf(
			`{"id":%d,"run_id":41,"name":"job-%03d","head_sha":%q,"status":"completed","conclusion":"success"}`,
			i+1, i, actionsRunHead))
	}

	for _, c := range []struct {
		name string
		page string
	}{
		{"an empty body", ``},
		{"a truncated document", `{"total_count":1,"jobs":[`},
		{"an array where the document should be", `[{"id":1,"run_id":41}]`},
		{"a jobs field that is not a list", `{"total_count":1,"jobs":{"id":1}}`},
		{"a total count below zero", `{"total_count":-1,"jobs":[]}`},
		{"more jobs on one page than a page holds", `{"total_count":101,"jobs":[` + strings.Join(hundredAndOne, ",") + `]}`},
	} {
		t.Run(c.name, func(t *testing.T) {
			reader, server := newActionsOutcomeReader(t)
			server.serveJobs(http.StatusOK, c.page)
			run, err := reader.Outcomes(context.Background(), 41)
			if !errors.Is(err, ErrActionsRunUnreadable) {
				t.Fatalf("err = %v, want ErrActionsRunUnreadable", err)
			}
			if !strings.Contains(err.Error(), "unreadable jobs document") {
				t.Fatalf("refusal does not name the jobs document: %v", err)
			}
			if len(run.Outcomes) != 0 || run.RunID != 0 || run.HeadSHA != "" {
				t.Fatalf("refused read still reported %#v", run)
			}
		})
	}
}

// The run itself gets the same treatment one endpoint earlier, and the two
// refusals are told apart by what they name: a run document that cannot be
// read is not a run whose jobs cannot be read.
func TestARunDocumentThisReaderCannotReadIsNamedSeparately(t *testing.T) {
	for _, c := range []struct {
		name string
		body string
	}{
		{"a truncated document", `{"id":41,`},
		{"an array where the run should be", `[41]`},
		{"a bare number", `41`},
	} {
		t.Run(c.name, func(t *testing.T) {
			reader, server := newActionsOutcomeReader(t)
			server.serveRun(http.StatusOK, c.body)
			_, err := reader.Outcomes(context.Background(), 41)
			if !errors.Is(err, ErrActionsRunUnreadable) {
				t.Fatalf("err = %v, want ErrActionsRunUnreadable", err)
			}
			if !strings.Contains(err.Error(), "unreadable run document") {
				t.Fatalf("refusal does not name the run document: %v", err)
			}
			_, paths, _ := server.seen()
			for _, path := range paths {
				if strings.HasSuffix(path, "/jobs") {
					t.Fatal("a run that could not be read was still asked for its jobs")
				}
			}
		})
	}
}

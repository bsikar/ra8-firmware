// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// halting accepts a fixed number of bytes and then refuses, the way a closed
// pipe or a full disk does partway through a page. It keeps what it took so a
// test can say which part of the page had already been written when the
// writer gave out.
type halting struct {
	budget int
	taken  bytes.Buffer
}

func (h *halting) Write(p []byte) (int, error) {
	if h.budget <= 0 {
		return 0, errors.New("the reader went away")
	}
	if len(p) > h.budget {
		took, _ := h.taken.Write(p[:h.budget])
		h.budget = 0
		return took, errors.New("the reader went away")
	}
	took, err := h.taken.Write(p)
	h.budget -= took
	return took, err
}

// wholePage is the comparison `shadow-compare` writes for a commit where one
// covered task ran and agreed, and a second covered task did not run here but
// was judged by Actions anyway. That is the page with all three of its parts:
// the report, the not-exercised tail, and the judged-without-a-run line.
func wholePage(t *testing.T) string {
	t.Helper()
	var out bytes.Buffer
	if err := githubShadowCompare(strings.NewReader(judgedWithoutARunInput(t)), &out); err != nil {
		t.Fatalf("comparison refused: %v", err)
	}
	return out.String()
}

func judgedWithoutARunInput(t *testing.T) string {
	t.Helper()
	task, other := twoCatalogTasks(t)
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile,
		writeCorrespondence(t, `{"`+task+`":"build","`+other+`":"test"}`))
	return `{"plane":[{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}],` +
		`"actions":[{"job":"build","head_sha":"` + shadowCompareHead + `","conclusion":"success"},` +
		`{"job":"test","head_sha":"` + shadowCompareHead + `","conclusion":"failure"}]}`
}

// A page that cannot be written is an error, at each of the three places the
// comparison writes. The verdict is the exit status and the page is written
// first, so a caller reading only the status must never get a clean one from a
// report nobody could read. Every part of the page is load-bearing that way,
// including the two tails, which carry the facts a reader cannot recover from
// the rest of the page.
func TestShadowCompareReportsAPageItCouldNotFinishWriting(t *testing.T) {
	page := wholePage(t)
	report := strings.Index(page, "\nnot exercised on this commit: ")
	if report < 0 {
		t.Fatalf("the page carries no unexercised tail to cut at:\n%s", page)
	}
	judged := strings.Index(page, "judged by Actions without a run on this side: ")
	if judged < 0 {
		t.Fatalf("the page carries no judged line to cut at:\n%s", page)
	}

	for name, budget := range map[string]int{
		"nothing written":    0,
		"report only":        report,
		"through both tails": judged,
	} {
		t.Run(name, func(t *testing.T) {
			out := &halting{budget: budget}
			err := githubShadowCompare(strings.NewReader(judgedWithoutARunInput(t)), out)
			if err == nil {
				t.Fatal("a page that could not be written was reported clean")
			}
			if !strings.Contains(err.Error(), "render shadow comparison") {
				t.Fatalf("err = %v; want the render named", err)
			}
			// The refusal must not read as a verdict about the
			// evidence, which is what an operator would act on.
			if strings.Contains(err.Error(), "not clean") {
				t.Fatalf("a write failure was reported as an unclean comparison: %v", err)
			}
			if out.taken.Len() > budget {
				t.Fatalf("wrote %d bytes past the budget of %d", out.taken.Len()-budget, budget)
			}
		})
	}
}

// The document is read under a limit, and a document over it is refused by
// size rather than parsed. The bound is on the read itself: an endpoint that
// floods this command must not be able to spend the box's memory on the way to
// being refused.
func TestShadowCompareRefusesADocumentLargerThanItWillRead(t *testing.T) {
	task := shadowCompareEnv(t, "build")
	pair := `{"task":"` + task + `","head_sha":"` + shadowCompareHead + `","observed":"success"}`
	var document strings.Builder
	document.WriteString(`{"plane":[` + pair)
	for document.Len() <= maxShadowComparisonBytes {
		document.WriteString("," + pair)
	}
	document.WriteString(`]}`)

	var out bytes.Buffer
	err := githubShadowCompare(strings.NewReader(document.String()), &out)
	if err == nil {
		t.Fatal("an oversized document was accepted")
	}
	if !strings.Contains(err.Error(), "read shadow observations") {
		t.Fatalf("err = %v; want the read named", err)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused document wrote a page: %q", out.String())
	}
}

// A correspondence file that is named and absent is a misconfigured
// deployment, not an unconfigured one. The two refusals read differently on
// purpose: one says to set the variable, the other says the file it points at
// could not be read.
func TestShadowCompareRefusesACorrespondenceFileItCannotRead(t *testing.T) {
	t.Setenv(github.EnvCheckRunMode, "shadow")
	t.Setenv(github.EnvShadowCorrespondenceFile, t.TempDir()+"/absent.json")

	var out bytes.Buffer
	err := githubShadowCompare(strings.NewReader(`{"plane":[]}`), &out)
	if err == nil {
		t.Fatal("an absent correspondence file was accepted")
	}
	if strings.Contains(err.Error(), "is not configured") {
		t.Fatalf("a named but unreadable file was reported as unconfigured: %v", err)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused comparison wrote a page: %q", out.String())
	}
}

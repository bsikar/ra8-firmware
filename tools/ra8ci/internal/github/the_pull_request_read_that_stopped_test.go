// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"testing"
)

// A pull request whose document never finished arriving must never answer as
// a head, because the evidence run would then report a commit nobody read.
// pull_request_head_reader_test.go pins the documents GitHub answers with;
// this pins the reads that stop partway.

// A dropped connection is a failed read, never a pull request without a head.
func TestAPullRequestReadThatDroppedIsNotAHeadlessPullRequest(t *testing.T) {
	dropped := newPullRequestHeadReader(t, hangUp)
	head, err := dropped.Head(context.Background(), 41)
	if err == nil || !strings.Contains(err.Error(), "read GitHub pull request") {
		t.Fatalf("Head = %+v, %v", head, err)
	}
	if head.Number != 0 || head.HeadSHA != "" || head.FromFork {
		t.Fatalf("a failed read carried a head: %+v", head)
	}

	// A body that stops short of its own Content-Length is the same
	// failure. The bytes that did arrive parse as nothing, and the
	// reader must not read the absent head repository as this one.
	truncated := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Length", "4096")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"number":41,"state":"open","head":{"sha":`))
		if flusher, ok := w.(http.Flusher); ok {
			flusher.Flush()
		}
		hangUp(w, r)
	})
	head, err = truncated.Head(context.Background(), 41)
	if err == nil || !errors.Is(err, ErrPullRequestUnreadable) {
		t.Fatalf("Head = %+v, %v", head, err)
	}
	if head.FromFork {
		t.Fatalf("a truncated read was called a fork: %+v", head)
	}
}

// A document past the response bound is refused rather than read up to the
// bound, and the bound itself is not the refusal.
func TestAPullRequestDocumentPastTheBoundIsRefused(t *testing.T) {
	oversized := newPullRequestHeadReader(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(strings.Repeat("x", maxPullRequestResponse+1)))
	})
	if _, err := oversized.Head(context.Background(), 41); err == nil ||
		!errors.Is(err, ErrPullRequestUnreadable) ||
		!strings.Contains(err.Error(), "unreadable or too large") {
		t.Fatalf("an oversized document: %v", err)
	}

	// Exactly at the bound the body is read, so the refusal that follows
	// is about the content and not the length. That ordering is what says
	// an honest document of that size would still be answered.
	atTheBound := newPullRequestHeadReader(t, func(w http.ResponseWriter, _ *http.Request) {
		document := pullRequestBody(41, "open", false, actionsRunHead, "ra8ci/dev", "bsikar/ra8-firmware")
		padding := maxPullRequestResponse - len(document)
		if padding < 0 {
			t.Fatal("the bound is smaller than one pull request document")
		}
		_, _ = w.Write([]byte(document + strings.Repeat(" ", padding)))
	})
	head, err := atTheBound.Head(context.Background(), 41)
	if err != nil {
		t.Fatalf("a document exactly at the bound was refused: %v", err)
	}
	if head.HeadSHA != actionsRunHead || head.FromFork {
		t.Fatalf("head at the bound = %+v", head)
	}
}

// Every non-200 is a refusal naming the status, so an operator can tell a
// pull request that is gone from one this App may not read.
func TestEveryRefusedPullRequestReadNamesItsStatus(t *testing.T) {
	for _, status := range []int{
		http.StatusMovedPermanently,
		http.StatusUnauthorized,
		http.StatusForbidden,
		http.StatusNotFound,
		http.StatusUnprocessableEntity,
		http.StatusTooManyRequests,
		http.StatusInternalServerError,
		http.StatusBadGateway,
		http.StatusServiceUnavailable,
	} {
		refusing := newPullRequestHeadReader(t, func(w http.ResponseWriter, _ *http.Request) {
			w.Header().Set("Location", "https://example.invalid/pulls/41")
			w.WriteHeader(status)
			_, _ = w.Write([]byte(pullRequestBody(41, "open", false, actionsRunHead, "ra8ci/dev", "bsikar/ra8-firmware")))
		})
		head, err := refusing.Head(context.Background(), 41)
		if err == nil || !errors.Is(err, ErrPullRequestUnreadable) {
			t.Fatalf("HTTP %d answered %+v, %v", status, head, err)
		}
		if !strings.Contains(err.Error(), http.StatusText(status)) &&
			!strings.Contains(err.Error(), "HTTP") {
			t.Fatalf("HTTP %d did not name its status: %v", status, err)
		}
		if head.HeadSHA != "" {
			t.Fatalf("HTTP %d carried a head: %+v", status, head)
		}
	}
}

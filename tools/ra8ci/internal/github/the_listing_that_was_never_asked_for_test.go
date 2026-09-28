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

// A commit's workflow runs are what the evidence selection reads to decide
// which pull request CI has actually finished with. An empty listing means
// "GitHub recorded no runs on this commit", so a listing that came back empty
// because the reader was incomplete, or because the token was never granted,
// would tell the selection CI has not started on a commit it has finished.

// A listing asked for without the parts it takes is refused, and nothing is
// asked of the forge.
func TestACommitListingWithoutItsPartsIsRefused(t *testing.T) {
	var absent *ActionsOutcomeReader
	listing, err := absent.RunsOn(context.Background(), actionsRunHead)
	if err == nil || !strings.Contains(err.Error(), "invalid commit workflow run request") {
		t.Fatalf("a listing from no reader answered %v", err)
	}
	if len(listing.Runs) != 0 || listing.HeadSHA != "" {
		t.Fatalf("a listing from no reader answered %+v", listing)
	}

	reader, server := newActionsOutcomeReader(t)
	var missing context.Context
	listing, err = reader.RunsOn(missing, actionsRunHead)
	if err == nil || !strings.Contains(err.Error(), "invalid commit workflow run request") {
		t.Fatalf("a listing with no context answered %v", err)
	}
	if len(listing.Runs) != 0 {
		t.Fatalf("a listing with no context answered %+v", listing)
	}
	if _, paths, _ := server.seen(); len(paths) != 0 {
		t.Fatalf("an incomplete listing reached the forge at %v", paths)
	}

	// A commit that is not a commit is refused on its own terms, so an
	// operator can tell a malformed SHA from a forge that would not answer.
	for _, notACommit := range []string{"", "HEAD", "dev", strings.Repeat("a", 39), strings.Repeat("a", 41), strings.Repeat("g", 40), strings.ToUpper(actionsRunHead) + "x"} {
		if _, err := reader.RunsOn(context.Background(), notACommit); !errors.Is(err, ErrInvalidCheckRunSHA) {
			t.Errorf("%q answered %v, want a refused commit", notACommit, err)
		}
	}
	if _, paths, _ := server.seen(); len(paths) != 0 {
		t.Fatalf("a refused commit reached the forge at %v", paths)
	}
}

// A token the forge will not mint stops the listing rather than answering an
// empty one, and the runs endpoint is never asked without a token.
func TestACommitListingWithoutATokenIsNotAnEmptyListing(t *testing.T) {
	reader, server := newActionsOutcomeReader(t)
	reader.tokens = mintingInstallation(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusUnauthorized)
	})

	listing, err := reader.RunsOn(context.Background(), actionsRunHead)
	if err == nil || !strings.Contains(err.Error(), "GitHub installation token endpoint returned HTTP 401") {
		t.Fatalf("a refused token answered %v", err)
	}
	if len(listing.Runs) != 0 || listing.HeadSHA != "" {
		t.Fatalf("a refused token answered the listing %+v", listing)
	}
	if _, paths, _ := server.seen(); len(paths) != 0 {
		t.Fatalf("the runs endpoint was asked at %v without a token", paths)
	}

	// The same reader, once its token is granted again, lists the commit:
	// the refusal is the mint's, not a reader that has given up.
	granted, _ := newActionsOutcomeReader(t)
	if _, err := granted.RunsOn(context.Background(), actionsRunHead); err != nil {
		t.Fatalf("a granted token answered %v", err)
	}
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package correlate

import (
	"context"
	"strings"
	"testing"
)

func TestTheRuleRefusesWhatAServerWouldNotEchoBack(t *testing.T) {
	for name, id := range map[string]string{
		"empty":      "",
		"space":      "ci job 4417",
		"newline":    "ci-4417\nX-Correlation-Id: someone-elses",
		"tab":        "ci-4417\t",
		"nul":        "ci-4417\x00",
		"non ascii":  "ci-4417-\u00e9",
		"slash":      "ci/4417",
		"over bound": strings.Repeat("a", MaxID+1),
	} {
		if Valid(id) {
			t.Errorf("%s accepted: %q", name, id)
		}
	}
	for name, id := range map[string]string{
		"minted":     New(),
		"uuid":       "018f3a2b-7c41-7a2e-8c9d-3b5f6a7c8d90",
		"ci job":     "ci-job-4417.attempt-2",
		"tilde":      "build~9",
		"at bound":   strings.Repeat("a", MaxID),
		"one letter": "a",
	} {
		if !Valid(id) {
			t.Errorf("%s refused: %q", name, id)
		}
	}
}

func TestAMintedIdentifierIsDistinctAndWellFormed(t *testing.T) {
	seen := map[string]bool{}
	for i := 0; i < 64; i++ {
		id := New()
		if !Valid(id) || len(id) != IDBytes*2 {
			t.Fatalf("minted identifier %d: %q", i, id)
		}
		if seen[id] {
			t.Fatalf("minted identifier %d repeated: %q", i, id)
		}
		seen[id] = true
	}
}

func TestPinningRefusesWhatItWillNotSend(t *testing.T) {
	ctx, ok := WithID(context.Background(), "ci job")
	if ok || IDFrom(ctx) != "" {
		t.Fatalf("a malformed identifier was pinned: ok=%v id=%q", ok, IDFrom(ctx))
	}
	ctx, ok = WithID(context.Background(), "ci-job-4417")
	if !ok || IDFrom(ctx) != "ci-job-4417" {
		t.Fatalf("a well-formed identifier was not pinned: ok=%v id=%q", ok, IDFrom(ctx))
	}
	if got := Outgoing(ctx); got != "ci-job-4417" {
		t.Fatalf("outgoing under a pin: got %q", got)
	}
}

func TestAnUnpinnedRequestGetsItsOwnThread(t *testing.T) {
	first, second := Outgoing(context.Background()), Outgoing(context.Background())
	if first == second {
		t.Fatalf("two unpinned requests shared one thread: %q", first)
	}
	if !Valid(first) || !Valid(second) {
		t.Fatalf("unpinned threads are malformed: %q %q", first, second)
	}
	if IDFrom(nil) != "" || Outgoing(nil) == "" { //nolint:staticcheck // a nil context must not panic here
		t.Fatal("a nil context must read as unpinned and still yield a thread")
	}
}

func TestServedPrefersWhatTheServerAdopted(t *testing.T) {
	if got := Served("from-the-header", "from-the-body"); got != "from-the-header" {
		t.Fatalf("header should win: got %q", got)
	}
	if got := Served("", "from-the-body"); got != "from-the-body" {
		t.Fatalf("body is the fallback: got %q", got)
	}
	if got := Served("not a thread", "also not a thread"); got != "" {
		t.Fatalf("a malformed served identifier was kept: %q", got)
	}
	if got := Served("", ""); got != "" {
		t.Fatalf("a thread was invented: %q", got)
	}
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"math"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	offsetRunID     = "00000000-0000-7000-8000-000000000001"
	offsetAttemptID = "00000000-0000-7000-8000-000000000002"
)

// stamped builds a page whose chunks are contiguous from after and carry the
// supplied offsets, with every other field the page needs to reach the offset
// rule: real digests, a known stream, non-empty bytes.
func stamped(after int64, offsets ...int64) store.LogPage {
	page := store.LogPage{AttemptID: offsetAttemptID, NextAfter: after}
	for index, offset := range offsets {
		data := []byte("line\n")
		sum := sha256.Sum256(data)
		page.Chunks = append(page.Chunks, store.LogRecord{
			Sequence:          after + int64(index) + 1,
			Stream:            "stdout",
			MonotonicOffsetNS: offset,
			SHA256:            hex.EncodeToString(sum[:]),
			DataBase64:        base64.StdEncoding.EncodeToString(data),
		})
	}
	page.NextAfter = after + int64(len(offsets))
	return page
}

// serving answers every log request with the supplied page, so a test can ask
// the real Logs door what it does with it.
func serving(t *testing.T, page store.LogPage) *Client {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode(page)
	}))
	t.Cleanup(server.Close)
	return testClient(server)
}

func TestAnAttemptStampingEveryChunkAtZeroIsServed(t *testing.T) {
	// The agent log path inserts monotonic_offset_ns = 0 for every chunk it
	// records, so an all-zero page is the ordinary shape and must pass.
	if err := checkOffsetsTheAttemptCouldStamp(stamped(0, 0, 0, 0, 0)); err != nil {
		t.Fatalf("an all-zero agent page was refused: %v", err)
	}
}

func TestOffsetsThatOnlyMoveForwardAreServed(t *testing.T) {
	if err := checkOffsetsTheAttemptCouldStamp(stamped(0, 0, 12, 12, 400, math.MaxInt64)); err != nil {
		t.Fatalf("a non-decreasing page was refused: %v", err)
	}
}

func TestAnOffsetBeforeTheAttemptBeganIsRefused(t *testing.T) {
	for _, offset := range []int64{-1, -1000, math.MinInt64} {
		if err := checkOffsetsTheAttemptCouldStamp(stamped(0, offset)); err == nil {
			t.Fatalf("offset %d accepted", offset)
		}
	}
}

func TestANegativeOffsetIsRefusedWhereverItSitsInThePage(t *testing.T) {
	// The first chunk has no predecessor to compare against, so a page whose
	// only bad offset is in the middle or at the end has to be caught by the
	// negative rule and the ordering rule, not by position.
	for name, page := range map[string]store.LogPage{
		"first":  stamped(0, -5, 10, 20),
		"middle": stamped(0, 10, -5, 20),
		"last":   stamped(0, 10, 20, -5),
	} {
		if err := checkOffsetsTheAttemptCouldStamp(page); err == nil {
			t.Fatalf("negative offset in %s position accepted", name)
		}
	}
}

func TestAnOffsetGoingBackwardsDownThePageIsRefused(t *testing.T) {
	if err := checkOffsetsTheAttemptCouldStamp(stamped(0, 100, 99)); err == nil {
		t.Fatal("an offset going backwards was accepted")
	}
	if err := checkOffsetsTheAttemptCouldStamp(stamped(0, 0, 900, 900, 12)); err == nil {
		t.Fatal("a late backwards step was accepted")
	}
}

func TestTheRefusalNamesTheChunkAndItsOffset(t *testing.T) {
	// The error is read by a person holding a cursor, so it has to say which
	// chunk to go back to.
	err := checkOffsetsTheAttemptCouldStamp(stamped(40, 10, 3))
	if err == nil {
		t.Fatal("backwards offset accepted")
	}
	for _, want := range []string{"42", "3", "41", "10"} {
		if !containsOffsetDetail(err.Error(), want) {
			t.Fatalf("error %q does not name %q", err, want)
		}
	}
}

func TestAnEmptyPageHasNoOffsetsToJudge(t *testing.T) {
	if err := checkOffsetsTheAttemptCouldStamp(store.LogPage{AttemptID: offsetAttemptID}); err != nil {
		t.Fatalf("an empty page was refused: %v", err)
	}
}

func TestLogsServesAPageWhoseOffsetsOnlyMoveForward(t *testing.T) {
	client := serving(t, stamped(0, 0, 250, 1000))
	page, err := client.Logs(context.Background(), offsetRunID, offsetAttemptID, 0, 3)
	if err != nil {
		t.Fatal(err)
	}
	if len(page.Chunks) != 3 || page.Chunks[2].MonotonicOffsetNS != 1000 {
		t.Fatalf("unexpected page: %+v", page)
	}
}

func TestLogsRefusesAPageStampedBeforeTheAttemptBegan(t *testing.T) {
	client := serving(t, stamped(0, -1))
	if _, err := client.Logs(context.Background(), offsetRunID, offsetAttemptID, 0, 1); err == nil {
		t.Fatal("a page stamped before the attempt began was served to the caller")
	}
}

func TestLogsRefusesAPageWhoseOffsetsGoBackwards(t *testing.T) {
	client := serving(t, stamped(0, 900, 100))
	if _, err := client.Logs(context.Background(), offsetRunID, offsetAttemptID, 0, 2); err == nil {
		t.Fatal("a page whose offsets go backwards was served to the caller")
	}
}

func TestTheStoreStillRefusesTheOffsetThisRuleRefuses(t *testing.T) {
	// A transcription of the far end's own rule: store.AttemptLogs refuses a
	// row with monotonic_offset_ns < 0 while reading rows it owns. If that
	// ever stops being true, this near-end rule is repeating a judgement the
	// store no longer makes and should be reconsidered rather than kept by
	// habit.
	const storeRefusesNegativeOffsets = true
	if !storeRefusesNegativeOffsets {
		t.Fatal("the store no longer refuses a negative offset; re-read this rule")
	}
	if err := checkOffsetsTheAttemptCouldStamp(stamped(0, -1)); err == nil {
		t.Fatal("this rule no longer refuses what the store refuses")
	}
}

func containsOffsetDetail(text, want string) bool {
	for index := 0; index+len(want) <= len(text); index++ {
		if text[index:index+len(want)] == want {
			return true
		}
	}
	return false
}

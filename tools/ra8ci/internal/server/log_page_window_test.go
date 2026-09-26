// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// chunksAt builds a page body from bare sequences. Nothing else on a chunk is
// read by the rule under test, so the fixtures stay the sequences themselves.
func chunksAt(sequences ...int64) []store.LogRecord {
	chunks := make([]store.LogRecord, 0, len(sequences))
	for _, sequence := range sequences {
		chunks = append(chunks, store.LogRecord{Sequence: sequence, Stream: "stdout"})
	}
	return chunks
}

func TestLogPageWindowJudgesEachShape(t *testing.T) {
	cases := []struct {
		name    string
		page    store.LogPage
		after   int64
		accepts bool
	}{
		{"one chunk just past the cursor", store.LogPage{Chunks: chunksAt(1), NextAfter: 1}, 0, true},
		{"a full ascending page", store.LogPage{Chunks: chunksAt(4, 5, 6), NextAfter: 6}, 3, true},
		{"gaps are allowed, order is not", store.LogPage{Chunks: chunksAt(4, 9, 11), NextAfter: 11}, 3, true},
		{"chunk sitting on the cursor", store.LogPage{Chunks: chunksAt(3), NextAfter: 3}, 3, false},
		{"chunk below the cursor", store.LogPage{Chunks: chunksAt(2), NextAfter: 2}, 3, false},
		{"first chunk below, last on the cursor", store.LogPage{Chunks: chunksAt(2, 5), NextAfter: 5}, 3, false},
		{"descending pair", store.LogPage{Chunks: chunksAt(5, 4), NextAfter: 4}, 3, false},
		{"one row out of order in the middle", store.LogPage{Chunks: chunksAt(4, 6, 5, 7), NextAfter: 7}, 3, false},
		{"the same chunk twice", store.LogPage{Chunks: chunksAt(4, 4), NextAfter: 4}, 3, false},
		{"cursor ahead of the last row", store.LogPage{Chunks: chunksAt(4, 5), NextAfter: 9}, 3, false},
		{"cursor behind the last row", store.LogPage{Chunks: chunksAt(4, 5), NextAfter: 4}, 3, false},
		{"empty page holding the cursor", store.LogPage{NextAfter: 3}, 3, true},
		{"empty page advancing the cursor", store.LogPage{NextAfter: 4}, 3, false},
		{"empty page claiming more", store.LogPage{NextAfter: 3, HasMore: true}, 3, false},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			if got := logPageFitsItsWindow(testCase.page, testCase.after); got != testCase.accepts {
				t.Fatalf("accepts=%v want %v for %+v after=%d", got, testCase.accepts, testCase.page, testCase.after)
			}
		})
	}
}

// The store's own read path is transcribed here rather than called: it needs a
// database. AttemptLogs selects seq > after in ascending order and refuses any
// row whose Sequence is not NextAfter+1, so every page it can build is
// contiguous from after+1. All of them have to pass, at every page size the
// door allows.
func TestLogPageWindowAcceptsEveryPageTheStoreCanBuild(t *testing.T) {
	for _, after := range []int64{0, 1, 7, 4095} {
		for size := 1; size <= store.MaxLogPageSize; size++ {
			sequences := make([]int64, 0, size)
			for index := 1; index <= size; index++ {
				sequences = append(sequences, after+int64(index))
			}
			page := store.LogPage{Chunks: chunksAt(sequences...), NextAfter: after + int64(size)}
			if !logPageFitsItsWindow(page, after) {
				t.Fatalf("refused a contiguous page: after=%d size=%d", after, size)
			}
			page.HasMore = true
			if !logPageFitsItsWindow(page, after) {
				t.Fatalf("has_more changed the verdict: after=%d size=%d", after, size)
			}
		}
	}
}

// Swapping one adjacent pair is the whole difference between the accepted page
// and the refused one, so the rule is reading the order and not some other
// property the fixtures happen to share.
func TestLogPageWindowSeparatesOnOrderAlone(t *testing.T) {
	ascending := store.LogPage{Chunks: chunksAt(4, 5, 6, 7), NextAfter: 7}
	if !logPageFitsItsWindow(ascending, 3) {
		t.Fatal("ascending page refused")
	}
	swapped := store.LogPage{Chunks: chunksAt(4, 6, 5, 7), NextAfter: 7}
	if logPageFitsItsWindow(swapped, 3) {
		t.Fatal("swapped pair accepted")
	}
}

// A chunk exactly on the cursor is the shape that loops: the reader pages with
// the NextAfter it was handed, gets the same row back, and never moves.
func TestLogPageWindowRefusesTheCursorThatNeverMoves(t *testing.T) {
	for _, after := range []int64{0, 1, 2, 3, 4096} {
		page := store.LogPage{Chunks: chunksAt(after), NextAfter: after}
		if logPageFitsItsWindow(page, after) {
			t.Fatalf("accepted a page that leaves the cursor at %d", after)
		}
	}
}

func TestRunLogsRefusesAnOutOfOrderStorePage(t *testing.T) {
	reader := &fakeRunLogReader{repository: "bsikar/ra8-firmware", page: store.LogPage{
		AttemptID: testAttemptID, Chunks: chunksAt(2, 4, 3), NextAfter: 3,
	}}
	api := &Server{logReader: reader, auth: &allowRunLogRead{}}
	request := httptest.NewRequest(http.MethodGet, "/v1/runs/"+testRunID+"/logs?attempt_id="+testAttemptID+"&after=1&limit=8", nil)
	request.SetPathValue("id", testRunID)
	response := httptest.NewRecorder()
	api.getRunLogs(response, request)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
}

func TestRunLogsRefusesAChunkTheReaderAlreadyHas(t *testing.T) {
	reader := &fakeRunLogReader{repository: "bsikar/ra8-firmware", page: store.LogPage{
		AttemptID: testAttemptID, Chunks: chunksAt(2, 5), NextAfter: 5,
	}}
	api := &Server{logReader: reader, auth: &allowRunLogRead{}}
	request := httptest.NewRequest(http.MethodGet, "/v1/runs/"+testRunID+"/logs?attempt_id="+testAttemptID+"&after=2&limit=8", nil)
	request.SetPathValue("id", testRunID)
	response := httptest.NewRecorder()
	api.getRunLogs(response, request)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
}

// The ordinary page still goes out, chunks and cursor untouched.
func TestRunLogsHandsOnAWellOrderedPage(t *testing.T) {
	reader := &fakeRunLogReader{repository: "bsikar/ra8-firmware", page: store.LogPage{
		AttemptID: testAttemptID, Chunks: chunksAt(3, 4, 5), NextAfter: 5, HasMore: true,
	}}
	api := &Server{logReader: reader, auth: &allowRunLogRead{}}
	request := httptest.NewRequest(http.MethodGet, "/v1/runs/"+testRunID+"/logs?attempt_id="+testAttemptID+"&after=2&limit=3", nil)
	request.SetPathValue("id", testRunID)
	response := httptest.NewRecorder()
	api.getRunLogs(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body store.LogPage
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body.NextAfter != 5 || !body.HasMore || len(body.Chunks) != 3 ||
		body.Chunks[0].Sequence != 3 || body.Chunks[2].Sequence != 5 {
		t.Fatalf("page changed on the way out: %+v", body)
	}
}

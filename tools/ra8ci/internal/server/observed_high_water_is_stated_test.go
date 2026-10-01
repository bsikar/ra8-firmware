package server

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const observePath = "/v1/boards/ek-ra8d2/agent/observe"

func observeBody(body string) *http.Request {
	return boardTestRequest("POST", observePath, body)
}

func TestObservedHighWaterIsStatedAcceptsAnyReportedMark(t *testing.T) {
	for _, mark := range []uint64{1, 2, 7, 1 << 32} {
		if !observedHighWaterIsStated(mark) {
			t.Fatalf("stated high-water %d was refused", mark)
		}
	}
}

func TestObservedHighWaterIsStatedRefusesTheUnwrittenValue(t *testing.T) {
	if observedHighWaterIsStated(0) {
		t.Fatal("an unstated high-water was accepted")
	}
}

func TestObserveDoorRefusesAnAbsentHighWater(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, observeBody(`{"expected_version":5}`))
	if w.Code != http.StatusBadRequest || f.applies != 0 {
		t.Fatalf("an omitted high-water reached the state machine: status=%d applies=%d", w.Code, f.applies)
	}
}

func TestObserveDoorRefusesAnExplicitZeroHighWater(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, observeBody(`{"expected_version":5,"high_water":0}`))
	if w.Code != http.StatusBadRequest || f.applies != 0 {
		t.Fatalf("a zero high-water reached the state machine: status=%d applies=%d", w.Code, f.applies)
	}
}

func TestObserveDoorRefusesANullHighWater(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, observeBody(`{"expected_version":5,"high_water":null}`))
	if w.Code != http.StatusBadRequest || f.applies != 0 {
		t.Fatalf("a null high-water reached the state machine: status=%d applies=%d", w.Code, f.applies)
	}
}

func TestObserveDoorPassesAStatedHighWaterThrough(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, observeBody(`{"expected_version":5,"high_water":3}`))
	command, ok := f.command.(board.ObserveAgentGeneration)
	if w.Code != http.StatusOK || f.applies != 1 || !ok || command.HighWater != 3 || f.version != 5 || f.neutral != nil {
		t.Fatalf("a stated observation was not passed through: status=%d applies=%d cmd=%#v version=%d", w.Code, f.applies, f.command, f.version)
	}
}

func TestObserveDoorStillRefusesUnknownAndTrailingFields(t *testing.T) {
	f := &fakeBoardStore{}
	for _, body := range []string{
		`{"expected_version":5,"high_water":3,"actor":"someone-else"}`,
		`{"expected_version":5,"high_water":3} {}`,
	} {
		w := httptest.NewRecorder()
		boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, observeBody(body))
		if w.Code != http.StatusBadRequest || f.applies != 0 {
			t.Fatalf("untrusted observation body reached the store: status=%d applies=%d body=%s", w.Code, f.applies, body)
		}
	}
}

func TestObserveDoorJudgesTheBodyOnlyAfterAuthorization(t *testing.T) {
	f := &fakeBoardStore{authorizeErr: store.ErrDenied}
	w := httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, observeBody(`{"expected_version":5,"high_water":0}`))
	if w.Code != http.StatusNotFound || f.audits != 1 || f.action != "board.agent.observe" || f.applies != 0 {
		t.Fatalf("a refused body answered before authorization: status=%d audits=%d action=%q", w.Code, f.audits, f.action)
	}
}

// The cost the door is refusing on behalf of: the state machine reads a
// high-water below the recorded mark as the database and the agent
// disagreeing, and takes the board out of service for it.
func TestUnstatedHighWaterWouldQuarantineAServedBoard(t *testing.T) {
	before := board.Snapshot{BoardID: "ek-ra8d2", Phase: board.Ready, Generation: 4, AgentHighWater: 4}
	after, _, err := board.Apply(before, board.ObserveAgentGeneration{Actor: "board-agent", HighWater: 0}, time.Now().UTC())
	if err != nil {
		t.Fatalf("observation errored instead of quarantining: %v", err)
	}
	if after.Phase != board.Quarantined {
		t.Fatalf("a zero high-water no longer quarantines, the door rule needs rereading: phase=%v", after.Phase)
	}
}

// And the cost of refusing it: nothing. On a board that has never seen an
// agent, zero was already a no-op, so the door turns away no report that
// could have said anything.
func TestUnstatedHighWaterSaysNothingOnAFreshBoard(t *testing.T) {
	before := board.Snapshot{BoardID: "ek-ra8d2", Phase: board.Ready, Generation: 0, AgentHighWater: 0}
	after, events, err := board.Apply(before, board.ObserveAgentGeneration{Actor: "board-agent", HighWater: 0}, time.Now().UTC())
	if err != nil || after.Phase != board.Ready || len(events) != 0 {
		t.Fatalf("zero was not a no-op on a fresh board: phase=%v events=%d err=%v", after.Phase, len(events), err)
	}
}

// A real divergence still reports: an agent ahead of the database states the
// number it installed, and is quarantined on that statement as before.
func TestAnAgentAheadOfTheDatabaseStillQuarantines(t *testing.T) {
	before := board.Snapshot{BoardID: "ek-ra8d2", Phase: board.Ready, Generation: 4, AgentHighWater: 4}
	after, _, err := board.Apply(before, board.ObserveAgentGeneration{Actor: "board-agent", HighWater: 5}, time.Now().UTC())
	if err != nil || after.Phase != board.Quarantined {
		t.Fatalf("a stated divergence stopped quarantining: phase=%v err=%v", after.Phase, err)
	}
}

func TestObserveDoorLeavesTheOtherAgentDoorsAlone(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/agent/ack",
		`{"expected_version":5,"lease_id":"`+boardTestLeaseID+`","generation":2,"installed_generation":2}`))
	if w.Code != http.StatusOK || f.applies != 1 {
		t.Fatalf("the ack door was caught by the observe rule: status=%d applies=%d", w.Code, f.applies)
	}
}

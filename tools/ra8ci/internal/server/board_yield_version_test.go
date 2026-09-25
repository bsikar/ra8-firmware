package server

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func yieldTestBudget() *fakeYieldBudget {
	return &fakeYieldBudget{budget: HandoffBudget{
		Cohort: yieldTestCohort(),
		Bounds: board.DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second},
	}}
}

func postYieldAtVersion(t *testing.T, mux *http.ServeMux, version string) *httptest.ResponseRecorder {
	t.Helper()
	w := httptest.NewRecorder()
	body := `{"expected_version":` + version + `,"waiter_id":"` + boardTestWaiterID + `"}`
	mux.ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/yield", body))
	return w
}

// A caller a version AHEAD of the read the plan was made from is the case
// that matters: the commit would succeed, and the promise recorded on the
// lease would have been estimated over a board state nobody planned from.
func TestYieldRefusesACallerAheadOfThePlannedVersion(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := yieldTestBudget()

	w := postYieldAtVersion(t, yieldTestMux(t, f, budget), "8")
	if w.Code != http.StatusConflict {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if f.applies != 0 {
		t.Fatalf("the board was asked to yield %d times against a version the plan never saw", f.applies)
	}
}

// A caller a version behind is the same disagreement, refused before the
// history read rather than by the commit a moment later.
func TestYieldRefusesACallerBehindThePlannedVersion(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := yieldTestBudget()

	w := postYieldAtVersion(t, yieldTestMux(t, f, budget), "6")
	if w.Code != http.StatusConflict {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if budget.calls != 0 {
		t.Fatalf("history was read %d times for a plan that cannot be committed", budget.calls)
	}
	if f.applies != 0 {
		t.Fatalf("the board was asked to yield %d times", f.applies)
	}
}

func TestYieldVersionRefusalNamesBothVersions(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}

	w := postYieldAtVersion(t, yieldTestMux(t, f, yieldTestBudget()), "8")
	body := w.Body.String()
	if !strings.Contains(body, "7") || !strings.Contains(body, "8") {
		t.Fatalf("refusal %q names neither the board's version nor the expected one", body)
	}
	if !strings.Contains(body, `"retryable":false`) {
		t.Fatalf("refusal %q reads as retryable; a stale read is retried with a fresh one", body)
	}
}

// The agreeing version still plans and commits, and it is the same version
// that reaches the store: the guard holds the door, it does not close it.
func TestYieldProceedsWhenTheCallerAgreesWithTheRead(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := yieldTestBudget()

	w := postYieldAtVersion(t, yieldTestMux(t, f, budget), "7")
	if w.Code != http.StatusOK {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if budget.calls != 1 || f.applies != 1 || f.version != 7 {
		t.Fatalf("budget %d, applies %d, committed version %d", budget.calls, f.applies, f.version)
	}
}

// Zero is a version like any other here. It is what a client that never read
// the board sends, and it is exactly the caller that must not be handed an
// estimate for a board it has not looked at.
func TestYieldRefusesACallerStatingNoVersion(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := yieldTestBudget()

	w := postYieldAtVersion(t, yieldTestMux(t, f, budget), "0")
	if w.Code != http.StatusConflict {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if budget.calls != 0 || f.applies != 0 {
		t.Fatalf("budget %d, applies %d, want neither", budget.calls, f.applies)
	}
}

// The version is judged after the waiter ID, so a malformed request is still
// answered as the bad request it is rather than as a conflict.
func TestYieldStillRefusesAnInvalidWaiterAheadOfTheVersion(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}

	w := httptest.NewRecorder()
	body := `{"expected_version":8,"waiter_id":"not-a-uuid"}`
	yieldTestMux(t, f, yieldTestBudget()).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/yield", body))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
}

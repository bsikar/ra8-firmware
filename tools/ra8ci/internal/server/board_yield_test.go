package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const boardTestWaiterID = "01996f90-3415-7cfe-8ff1-600058131b00"

type fakeYieldBudget struct {
	budget    HandoffBudget
	err       error
	calls     int
	sawPhase  board.Phase
	sawLeases int
}

func (f *fakeYieldBudget) HandoffBudget(_ context.Context, s board.Snapshot) (HandoffBudget, error) {
	f.calls++
	f.sawPhase = s.Phase
	if s.Lease != nil {
		f.sawLeases++
	}
	return f.budget, f.err
}

func yieldTestCohort() board.YieldCohort {
	return board.YieldCohort{
		BoardID:         "ek-ra8d2",
		BoardModel:      "EK-RA8D1",
		FixtureRevision: "rev-c",
		TaskName:        "hil-smoke",
		CatalogDigest:   strings.Repeat("a", 64),
	}
}

// yieldTestBoard is an active CI-held board with one higher-class waiter
// queued, the only shape in which a yield may be asked for at all.
func yieldTestBoard(waiterClass board.Class) board.Snapshot {
	granted := time.Date(2026, 9, 24, 17, 0, 0, 0, time.UTC)
	return board.Snapshot{
		BoardID:        "ek-ra8d2",
		Phase:          board.Active,
		Generation:     3,
		AgentHighWater: 3,
		Version:        7,
		NextSequence:   2,
		Lease: &board.Lease{
			ID: boardTestLeaseID, WaiterID: boardTestRequestID, Holder: "ci",
			Class: board.ClassCI, Reason: "nightly", Generation: 3, DeadlineVersion: 1,
			GrantedAt: granted, ExpiresAt: granted.Add(time.Hour), RequestedDuration: time.Hour,
		},
		Queue: []board.Waiter{{
			ID: boardTestWaiterID, LeaseID: boardTestProofID, Holder: "brighton",
			Class: waiterClass, Reason: "debugging a lockup", Duration: 30 * time.Minute,
			Sequence: 1, QueuedAt: granted.Add(time.Minute),
		}},
	}
}

func yieldTestMux(t *testing.T, f *fakeBoardStore, budget BoardYieldBudget) *http.ServeMux {
	t.Helper()
	mux := http.NewServeMux()
	if err := RegisterBoardRoutes(mux, f, nil, "bsikar/ra8-firmware", BoardPolicy{YieldBudget: budget}); err != nil {
		t.Fatal(err)
	}
	return mux
}

func postYield(t *testing.T, mux *http.ServeMux, waiterID string) *httptest.ResponseRecorder {
	t.Helper()
	w := httptest.NewRecorder()
	body := `{"expected_version":7,"waiter_id":"` + waiterID + `"}`
	mux.ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/yield", body))
	return w
}

func decodeYield(t *testing.T, w *httptest.ResponseRecorder) struct {
	Snapshot board.Snapshot `json:"snapshot"`
	Plan     struct {
		Known             bool       `json:"known"`
		Dispatch          string     `json:"dispatch"`
		Outstanding       bool       `json:"outstanding"`
		Overdue           bool       `json:"overdue"`
		RequestedAt       time.Time  `json:"requested_at"`
		ExpectedNeutralAt *time.Time `json:"expected_neutral_at"`
		TargetSeconds     float64    `json:"target_seconds"`
		Explain           string     `json:"explain"`
		Estimate          struct {
			Source        string  `json:"source"`
			Samples       int     `json:"samples"`
			Censored      int     `json:"censored"`
			MarginSeconds float64 `json:"margin_seconds"`
			TaskName      string  `json:"task_name"`
		} `json:"estimate"`
	} `json:"plan"`
} {
	t.Helper()
	var out struct {
		Snapshot board.Snapshot `json:"snapshot"`
		Plan     struct {
			Known             bool       `json:"known"`
			Dispatch          string     `json:"dispatch"`
			Outstanding       bool       `json:"outstanding"`
			Overdue           bool       `json:"overdue"`
			RequestedAt       time.Time  `json:"requested_at"`
			ExpectedNeutralAt *time.Time `json:"expected_neutral_at"`
			TargetSeconds     float64    `json:"target_seconds"`
			Explain           string     `json:"explain"`
			Estimate          struct {
				Source        string  `json:"source"`
				Samples       int     `json:"samples"`
				Censored      int     `json:"censored"`
				MarginSeconds float64 `json:"margin_seconds"`
				TaskName      string  `json:"task_name"`
			} `json:"estimate"`
		} `json:"plan"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &out); err != nil {
		t.Fatalf("decode yield response: %v (%s)", err, w.Body.String())
	}
	return out
}

func TestYieldAsksTheBoardAndShowsTheDeclaredEstimate(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := &fakeYieldBudget{budget: HandoffBudget{
		Cohort: yieldTestCohort(),
		Bounds: board.DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second},
	}}
	w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
	if w.Code != http.StatusOK {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	command, ok := f.command.(board.RequestYield)
	if !ok {
		t.Fatalf("applied %T, want board.RequestYield", f.command)
	}
	if command.WaiterID != boardTestWaiterID {
		t.Fatalf("asked on behalf of %q", command.WaiterID)
	}
	if f.version != 7 {
		t.Fatalf("expected version %d reached the store as %d", 7, f.version)
	}
	if budget.calls != 1 || budget.sawLeases != 1 {
		t.Fatalf("budget consulted %d times, saw %d leases", budget.calls, budget.sawLeases)
	}
	out := decodeYield(t, w)
	if !out.Plan.Known || out.Plan.TargetSeconds != 30 {
		t.Fatalf("plan target %v (known %v), want the 30s declared safety bound",
			out.Plan.TargetSeconds, out.Plan.Known)
	}
	if out.Plan.Estimate.Source != "declared" || out.Plan.Estimate.Samples != 0 {
		t.Fatalf("estimate source %q over %d samples, want the declared bounds",
			out.Plan.Estimate.Source, out.Plan.Estimate.Samples)
	}
	if out.Plan.Dispatch != string(board.YieldOperator) {
		t.Fatalf("dispatch %q, want operator for a queued human", out.Plan.Dispatch)
	}
	if out.Plan.ExpectedNeutralAt == nil ||
		!out.Plan.ExpectedNeutralAt.Equal(out.Plan.RequestedAt.Add(30*time.Second)) {
		t.Fatalf("expected-neutral %v is not the request plus the target", out.Plan.ExpectedNeutralAt)
	}
	if out.Plan.Outstanding || out.Plan.Overdue {
		t.Fatal("a yield just asked for is neither outstanding nor overdue")
	}
	if out.Plan.Estimate.TaskName != "hil-smoke" {
		t.Fatalf("cohort task %q did not reach the requester", out.Plan.Estimate.TaskName)
	}
}

// The requester is told what the estimate rests on, not only its number.
func TestYieldEstimateReportsTheHistoryItRestsOn(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	cohort := yieldTestCohort()
	now := time.Now().UTC()
	samples := make([]board.YieldSample, 0, 7)
	for i := range 6 {
		at := now.Add(-time.Duration(i+1) * time.Hour)
		// One measurement per lease, which is what the estimator is held
		// to: six repeats of one lease would weight a single handoff six
		// times in the quantile.
		samples = append(samples, board.YieldSample{
			Cohort: cohort, LeaseID: fmt.Sprintf("%s-%d", boardTestLeaseID, i), WaiterID: boardTestRequestID,
			RequestedAt: at, NeutralAt: at.Add(time.Duration(40+i) * time.Second),
		})
	}
	samples = append(samples, board.YieldSample{
		Cohort: cohort, LeaseID: boardTestLeaseID, WaiterID: boardTestRequestID,
		RequestedAt: now.Add(-9 * time.Hour), ExclusionReason: board.YieldExcludedNoReceipt,
	})
	budget := &fakeYieldBudget{budget: HandoffBudget{
		Cohort:  cohort,
		Bounds:  board.DeclaredHandoffBounds{SafeStepBound: 5 * time.Second, RestoreProbeBound: 5 * time.Second},
		Samples: samples,
	}}
	w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
	if w.Code != http.StatusOK {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	out := decodeYield(t, w)
	if out.Plan.Estimate.Source != "history" || out.Plan.Estimate.Samples != 6 || out.Plan.Estimate.Censored != 1 {
		t.Fatalf("estimate %q over %d samples with %d censored, want history over 6 with 1 censored",
			out.Plan.Estimate.Source, out.Plan.Estimate.Samples, out.Plan.Estimate.Censored)
	}
	// 45s slowest of six, nearest rank at 0.95, plus the 5s margin.
	if out.Plan.TargetSeconds != 50 {
		t.Fatalf("target %vs, want the 45s quantile plus the 5s margin", out.Plan.TargetSeconds)
	}
	if out.Plan.Estimate.MarginSeconds != board.HandoffMargin.Seconds() {
		t.Fatalf("margin %vs did not reach the requester", out.Plan.Estimate.MarginSeconds)
	}
	if !strings.Contains(out.Plan.Explain, "hil-smoke") {
		t.Fatalf("provenance line %q does not name the cohort", out.Plan.Explain)
	}
}

// The refusal that makes this endpoint the planning door: a scheduler may not
// ask for a board back when nothing can say how long the handoff will take,
// and the board is left untouched when it tries.
func TestYieldRefusesAnAutomaticRequestWithNoDeclaredBounds(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	f.board.Queue[0].Class = board.ClassHuman
	f.board.Lease.Class = board.ClassCI
	// An agent-class waiter outranks nothing here, so use a CI waiter over an
	// AI holder: automatic dispatch, admission satisfied, bounds undeclared.
	f.board.Lease.Class = board.ClassAI
	f.board.Queue[0].Class = board.ClassCI
	budget := &fakeYieldBudget{budget: HandoffBudget{Cohort: yieldTestCohort()}}
	w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if f.applies != 0 {
		t.Fatalf("the board was asked to yield %d times despite the refusal", f.applies)
	}
	if !strings.Contains(w.Body.String(), "declares no safe-step") {
		t.Fatalf("refusal %q does not say what is missing", w.Body.String())
	}
}

// The same board, the same missing bounds, asked for by a person: allowed,
// and the ETA is reported as unknown rather than invented.
func TestYieldLetsAPersonProceedWithAnUnknownETA(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := &fakeYieldBudget{budget: HandoffBudget{Cohort: yieldTestCohort()}}
	w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
	if w.Code != http.StatusOK {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if f.applies != 1 {
		t.Fatalf("applied %d commands, want the yield asked for once", f.applies)
	}
	out := decodeYield(t, w)
	if out.Plan.Known || out.Plan.TargetSeconds != 0 || out.Plan.ExpectedNeutralAt != nil {
		t.Fatalf("unknown ETA carried a number: known=%v target=%v neutral=%v",
			out.Plan.Known, out.Plan.TargetSeconds, out.Plan.ExpectedNeutralAt)
	}
	if out.Plan.Estimate.Source != string(board.HandoffUnknown) {
		t.Fatalf("estimate source %q, want unknown", out.Plan.Estimate.Source)
	}
	if !strings.Contains(out.Plan.Explain, "ETA unknown") {
		t.Fatalf("explanation %q does not say the ETA is unknown", out.Plan.Explain)
	}
}

// The dispatch standard comes from board state, so a requester cannot pick
// the laxer one: the body carries no dispatch field at all and a body that
// tries to add one is refused before anything is applied.
func TestYieldTakesTheDispatchFromBoardStateNotTheRequester(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := &fakeYieldBudget{budget: HandoffBudget{Cohort: yieldTestCohort()}}
	mux := yieldTestMux(t, f, budget)
	w := httptest.NewRecorder()
	body := `{"expected_version":7,"waiter_id":"` + boardTestWaiterID + `","dispatch":"operator"}`
	mux.ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/yield", body))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if f.applies != 0 {
		t.Fatal("an unknown field reached the board")
	}
	agent := yieldTestBoard(board.ClassCI)
	agent.Lease.Class = board.ClassAI
	if got := yieldDispatch(agent, boardTestWaiterID); got != board.YieldAutomatic {
		t.Fatalf("a CI waiter dispatched as %q", got)
	}
	if got := yieldDispatch(agent, boardTestRequestID); got != board.YieldAutomatic {
		t.Fatalf("an unqueued waiter dispatched as %q, want the stricter standard", got)
	}
}

// The plan and the commit share one clock, so the ETA a requester is shown is
// anchored to the instant the lease records, not to a slightly later one.
func TestYieldAnchorsTheETAToTheRecordedRequest(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := &fakeYieldBudget{budget: HandoffBudget{
		Cohort: yieldTestCohort(),
		Bounds: board.DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second},
	}}
	before := time.Now().UTC()
	w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
	after := time.Now().UTC()
	if w.Code != http.StatusOK {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	out := decodeYield(t, w)
	if out.Plan.RequestedAt.Before(before) || out.Plan.RequestedAt.After(after) {
		t.Fatalf("plan anchored at %v, outside the request window", out.Plan.RequestedAt)
	}

	// A second ask while the yield is outstanding must not move the deadline.
	outstanding := yieldTestBoard(board.ClassHuman)
	outstanding.Phase = board.YieldRequested
	outstanding.Lease.YieldRequestedAt = out.Plan.RequestedAt.Add(-90 * time.Second)
	f2 := &fakeBoardStore{board: outstanding}
	w2 := postYield(t, yieldTestMux(t, f2, budget), boardTestWaiterID)
	if w2.Code != http.StatusOK {
		t.Fatalf("status %d: %s", w2.Code, w2.Body.String())
	}
	again := decodeYield(t, w2)
	if !again.Plan.Outstanding {
		t.Fatal("a yield already asked for was reported as fresh")
	}
	if !again.Plan.RequestedAt.Equal(outstanding.Lease.YieldRequestedAt) {
		t.Fatalf("outstanding plan re-anchored to %v", again.Plan.RequestedAt)
	}
	if !again.Plan.Overdue {
		t.Fatal("a handoff 90s past a 30s target was not reported overdue")
	}
}

func TestYieldRefusesWhateverTheBoardWouldRefuse(t *testing.T) {
	budget := &fakeYieldBudget{budget: HandoffBudget{
		Cohort: yieldTestCohort(),
		Bounds: board.DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second},
	}}
	idle := yieldTestBoard(board.ClassHuman)
	idle.Phase = board.Ready
	idle.Lease = nil
	outranked := yieldTestBoard(board.ClassAI)
	for _, c := range []struct {
		name  string
		state board.Snapshot
		want  int
	}{
		{"no holder to ask", idle, http.StatusConflict},
		{"waiter does not outrank the holder", outranked, http.StatusForbidden},
	} {
		t.Run(c.name, func(t *testing.T) {
			f := &fakeBoardStore{board: c.state}
			w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
			if w.Code != c.want {
				t.Fatalf("status %d, want %d: %s", w.Code, c.want, w.Body.String())
			}
			if f.applies != 0 {
				t.Fatal("a refused yield still reached the board")
			}
		})
	}
}

// A deployment with no budget configured leaves the door closed rather than
// asking for a yield with the estimate quietly skipped.
func TestYieldIsClosedWithoutAConfiguredBudget(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	w := postYield(t, boardTestMux(t, f, nil), boardTestWaiterID)
	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if f.applies != 0 || f.reads != 0 {
		t.Fatalf("board touched without a budget: %d applies, %d reads", f.applies, f.reads)
	}
}

func TestYieldValidatesTheWaiterAndAuditsDenial(t *testing.T) {
	budget := &fakeYieldBudget{budget: HandoffBudget{Cohort: yieldTestCohort()}}
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	w := postYield(t, yieldTestMux(t, f, budget), "not-a-uuid")
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if budget.calls != 0 {
		t.Fatal("an invalid waiter ID reached the budget")
	}

	denied := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman), authorizeErr: store.ErrDenied}
	w = postYield(t, yieldTestMux(t, denied, budget), boardTestWaiterID)
	if w.Code != http.StatusNotFound {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if denied.action != "board.yield" {
		t.Fatalf("denial audited as %q", denied.action)
	}
}

// A budget the store cannot produce is not silently replaced by one with no
// history: the yield is refused and nothing is asked of the board.
func TestYieldRefusesWhenTheBudgetIsUnavailable(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman)}
	budget := &fakeYieldBudget{err: errors.New("history unavailable: " + store.ErrUnavailable.Error())}
	budget.err = store.ErrUnavailable
	w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("status %d: %s", w.Code, w.Body.String())
	}
	if f.applies != 0 {
		t.Fatal("the board was asked to yield with no budget behind it")
	}
}

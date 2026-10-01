// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The reviewed catalog compiled into this binary declares no HIL task at all:
// every one of its definitions is safe-local, so the door that answers a HIL
// observation history can never get past its own "is this a HIL task for this
// board" check with the real catalog behind it. That is why the existing tests
// for it stop at the refusal.
//
// This file supplies the missing half. catalog.Parse validates a manifest
// against the SHA-256 of its canonical form, and catalog.CanonicalJSON is
// exported, so a test can mint a manifest the catalog will genuinely accept
// rather than reaching past the validation with a hand-built struct. The task
// below is therefore a real reviewed definition, held to every admission rule
// the shipped ones are, which is what makes the assertions about the door
// downstream of it worth anything.

const hilBoardID = "ek-ra8d2"

// mintedHILCatalog is a reviewed catalog holding one HIL task for the board the
// board tests use. Every bound is chosen to satisfy the reviewed-task rules
// rather than to be pretty: the restore-and-probe covers the flash restore, the
// indivisible step covers the observation timeout, and the safety maximum
// covers both.
func mintedHILCatalog(t *testing.T) *catalog.Catalog {
	t.Helper()
	raw := []byte(`{
  "schema_version": 1,
  "tasks": [
    {
      "name": "hil-alive-probe",
      "version": 1,
      "tier": "required",
      "scope": "hil",
      "os": ["linux"],
      "capabilities": [],
      "args_schema": {"positional": [], "flags": []},
      "deadline_seconds": 1800,
      "board_policy": "exclusive",
      "steps": [
        {"name": "observe", "program": "bash", "args": ["scripts/ci.sh", "--gate", "mcdc"]}
      ],
      "outputs": [],
      "retry": {"max_attempts": 1},
      "resource_hints": {},
      "hil": {
        "board_id": "` + hilBoardID + `",
        "board_model": "EK-RA8D2",
        "manifest_path": "examples/hil_alive/hil.conf",
        "program_family": "hil-alive",
        "mode": "alive",
        "observation_step": "observe",
        "flash_restore_seconds": 20,
        "timeout_declared": true,
        "timeout_seconds": 30,
        "safety_maximum_seconds": 120,
        "handoff_safe_step_seconds": 60,
        "handoff_restore_probe_seconds": 30
      }
    }
  ]
}`)
	canonical, err := catalog.CanonicalJSON(raw)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(canonical)
	cat, err := catalog.Parse(raw, hex.EncodeToString(sum[:]))
	if err != nil {
		t.Fatalf("the minted HIL manifest is not a reviewed catalog: %v", err)
	}
	task, found := cat.Task("hil-alive-probe")
	if !found || task.Scope != "hil" || task.HIL == nil || task.HIL.BoardID != hilBoardID {
		t.Fatalf("the minted catalog does not declare the HIL task: found=%v", found)
	}
	return cat
}

// historyBoardStore is the plain board fake plus the one durable method the
// history door type-asserts for. It EMBEDS the fake rather than replacing it,
// so the plain fake stays a store that does NOT satisfy this interface and the
// 503 assertions written against it keep holding.
type historyBoardStore struct {
	*fakeBoardStore
	workload     hilspec.Workload
	observations []hilspec.HistoricalObservation
	err          error
	asked        int
	definition   catalog.HILTask
}

func (h *historyBoardStore) BoardHILObservations(_ context.Context, _ store.BoardActor, definition catalog.HILTask) (hilspec.Workload, []hilspec.HistoricalObservation, error) {
	h.asked++
	h.definition = definition
	return h.workload, h.observations, h.err
}

func historyMux(t *testing.T, st BoardStore, cat *catalog.Catalog) *http.ServeMux {
	t.Helper()
	mux := http.NewServeMux()
	if err := RegisterBoardRoutes(mux, st, nil, "bsikar/ra8-firmware", BoardPolicy{Catalog: cat}); err != nil {
		t.Fatal(err)
	}
	return mux
}

func historyRequest(taskName string) *http.Request {
	return boardTestRequest("POST", "/v1/boards/"+hilBoardID+"/hil-observations",
		`{"task_name":"`+taskName+`"}`)
}

// TestHILHistoryAnswersTheWorkloadItWasAskedFor is the door's happy path, which
// no test could reach before this file existed. The definition handed to the
// store is the catalog's own, not the caller's request, which is the whole point
// of naming a task rather than describing one: a client cannot ask for
// observations filed under a board or manifest it made up.
func TestHILHistoryAnswersTheWorkloadItWasAskedFor(t *testing.T) {
	cat := mintedHILCatalog(t)
	st := &historyBoardStore{fakeBoardStore: &fakeBoardStore{},
		observations: make([]hilspec.HistoricalObservation, 3)}
	w := httptest.NewRecorder()
	historyMux(t, st, cat).ServeHTTP(w, historyRequest("hil-alive-probe"))
	if w.Code != http.StatusOK {
		t.Fatalf("status %d, want 200: %s", w.Code, w.Body.String())
	}
	if st.asked != 1 {
		t.Fatalf("asked=%d, want one read", st.asked)
	}
	if st.definition.BoardID != hilBoardID || st.definition.ManifestPath != "examples/hil_alive/hil.conf" ||
		st.definition.ProgramFamily != "hil-alive" || st.definition.Mode != "alive" {
		t.Fatalf("the store was handed a definition the catalog does not declare: %+v", st.definition)
	}
	var answer struct {
		TaskName     string                          `json:"task_name"`
		Observations []hilspec.HistoricalObservation `json:"observations"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &answer); err != nil {
		t.Fatal(err)
	}
	// The catalog's name, so a caller cannot have its own spelling echoed back
	// as though the plane agreed to it.
	if answer.TaskName != "hil-alive-probe" {
		t.Fatalf("task name %q, want the catalog's own", answer.TaskName)
	}
	if len(answer.Observations) != 3 {
		t.Fatalf("observations %d, want the three the store filed", len(answer.Observations))
	}
}

// TestHILHistoryCarriesTheStoreRefusalItMet pins the read failure. The history
// is evidence a scheduler plans against, so a store that cannot produce it is
// reported rather than answered with an empty history, which would read as a
// board that has never been observed.
func TestHILHistoryCarriesTheStoreRefusalItMet(t *testing.T) {
	cat := mintedHILCatalog(t)
	st := &historyBoardStore{fakeBoardStore: &fakeBoardStore{}, err: store.ErrNotFound}
	w := httptest.NewRecorder()
	historyMux(t, st, cat).ServeHTTP(w, historyRequest("hil-alive-probe"))
	if w.Code != http.StatusNotFound {
		t.Fatalf("status %d, want 404", w.Code)
	}
	if st.asked != 1 {
		t.Fatalf("asked=%d, want the read that failed", st.asked)
	}
}

// TestHILHistoryWithoutADurableStoreSaysSoAfterItJudgesTheTask pins the order of
// the door's two unavailable answers against its argument check. A store that
// cannot file observations is a deployment gap and retryable; a task that is not
// a HIL definition for this board is the caller's error and is not. The task is
// judged FIRST, so a client naming a task that does not exist is told that,
// rather than being told to retry against a plane that would never have
// answered it.
func TestHILHistoryWithoutADurableStoreSaysSoAfterItJudgesTheTask(t *testing.T) {
	cat := mintedHILCatalog(t)
	plain := &fakeBoardStore{} // satisfies BoardStore only: no observations method
	w := httptest.NewRecorder()
	historyMux(t, plain, cat).ServeHTTP(w, historyRequest("hil-alive-probe"))
	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("status %d, want 503 for a store that cannot file observations", w.Code)
	}

	// Same store, a task the catalog does not declare: the caller's error wins,
	// because it is decided before the store is ever type-asserted.
	w = httptest.NewRecorder()
	historyMux(t, plain, cat).ServeHTTP(w, historyRequest("no-such-task"))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d, want 400 for an undeclared task ahead of the missing store", w.Code)
	}
}

// TestHILHistoryRefusesATaskThatIsNotThisBoards pins the check that makes the
// board in the path authoritative. The caller is authorized for one board, so a
// HIL task belonging to another must not be readable through it even though the
// task itself is a real reviewed definition.
func TestHILHistoryRefusesATaskThatIsNotThisBoards(t *testing.T) {
	cat := mintedHILCatalog(t)
	st := &historyBoardStore{fakeBoardStore: &fakeBoardStore{}}
	w := httptest.NewRecorder()
	historyMux(t, st, cat).ServeHTTP(w,
		boardTestRequest("POST", "/v1/boards/ek-ra8m1/hil-observations", `{"task_name":"hil-alive-probe"}`))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d, want 400 for another board's HIL task", w.Code)
	}
	if st.asked != 0 {
		t.Fatalf("asked=%d, want the store left untouched", st.asked)
	}
}

// TestMintedHILCatalogIsHeldToTheReviewedRules is the guard on the fixture
// above. A manifest that stopped satisfying the admission rules would silently
// turn every test in this file into a test of the refusal path again, so the
// bounds that matter are asserted here, where the failure names itself.
func TestMintedHILCatalogIsHeldToTheReviewedRules(t *testing.T) {
	task, found := mintedHILCatalog(t).Task("hil-alive-probe")
	if !found {
		t.Fatal("the minted task is missing")
	}
	hil := task.HIL
	if !hil.HandoffBoundsDeclared() {
		t.Fatal("the minted task declares no handoff bounds, so the yield path reads a different task")
	}
	if hil.HandoffRestoreProbeSeconds < hil.FlashRestoreSeconds {
		t.Fatal("restore-and-probe under the flash restore")
	}
	if hil.HandoffSafeStepSeconds < hil.TimeoutSeconds {
		t.Fatal("indivisible step under the observation timeout")
	}
	if hil.SafetyMaximumSeconds < hil.HandoffSafeStepSeconds || hil.TimeoutSeconds > task.DeadlineSeconds {
		t.Fatal("the minted bounds do not nest")
	}
	if err := catalog.ValidateHILTaskMetadata(*hil); err != nil {
		t.Fatalf("the minted HIL block is not reviewable: %v", err)
	}
	// Parse is what admits it, so a digest that does not match the canonical
	// form must refuse rather than fall back to trusting the bytes.
	if _, err := catalog.Parse([]byte(`{"schema_version":1,"tasks":[]}`), hex.EncodeToString(make([]byte, 32))); err == nil {
		t.Fatal("a manifest was parsed against a digest that is not its own")
	} else if errors.Is(err, nil) {
		t.Fatal("unreachable")
	}
}

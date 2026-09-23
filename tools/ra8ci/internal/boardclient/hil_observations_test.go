package boardclient

import (
	"context"
	"net/http"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

func TestHILObservationsReturnsApprovedCohortWithoutSamples(t *testing.T) {
	task := catalog.Task{Name: "uart-demo", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "exclusive",
		Retry: catalog.RetryPolicy{MaxAttempts: 1}, Steps: []catalog.Step{{Name: "observe", Program: "noop"}},
		HIL: &catalog.HILTask{BoardID: "ek-ra8d2", BoardModel: "EK-RA8D2",
			ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/uart_hello/hil.conf",
			ProgramFamily: "uart-hello", Mode: "uart_scrape", ObservationStep: "observe", FlashRestoreSeconds: 10}}
	workload := hilspec.Workload{ManifestPath: task.HIL.ManifestPath, BoardModel: task.HIL.BoardModel,
		FixtureRevision: "fixture-v2", ProfileSHA256: "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
		ProgramFamily: task.HIL.ProgramFamily, Mode: hilspec.Mode(task.HIL.Mode)}
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/v1/boards/ek-ra8d2/hil-observations" {
			t.Errorf("unexpected HIL history route: %s %s", r.Method, r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
			return
		}
		jsonResponse(w, http.StatusOK, map[string]any{"task_name": task.Name, "workload": workload, "observations": []any{}})
	})
	defer closeServer()
	got, rows, err := c.HILObservations(context.Background(), "ek-ra8d2", task)
	if err != nil || got != workload || len(rows) != 0 {
		t.Fatalf("empty HIL history lost its approved cohort: cohort=%+v rows=%+v err=%v", got, rows, err)
	}
	bad, closeBad := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		wrong := workload
		wrong.BoardModel = "other-board"
		jsonResponse(w, http.StatusOK, map[string]any{"task_name": task.Name, "workload": wrong, "observations": []any{}})
	})
	defer closeBad()
	if _, _, err := bad.HILObservations(context.Background(), "ek-ra8d2", task); err == nil {
		t.Fatal("history response for another board model was accepted")
	}
}

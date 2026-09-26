package agent

import (
	"context"
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// A 200 carrying a body this agent cannot read is the plane having answered,
// so the evidence is not offered again. Before this the decode failure came
// back as status 0, the value reserved for a call that never reached the
// plane, and one unreadable answer cost three offers and two backoffs out of
// the task's own budget.
func TestAnUnreadableAnswerIsNotOfferedAgain(t *testing.T) {
	for name, body := range map[string]string{
		"not JSON":          "<html>gateway</html>",
		"unknown field":     `{"schema_version":1,"surprise":true}`,
		"trailing document": `{"schema_version":1}{"schema_version":1}`,
	} {
		assignment := testAssignment()
		var calls atomic.Int64
		agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
			calls.Add(1)
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(body))
		})
		uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
		_, err := uploader.write("format-check", "stdout", []byte("x"))
		server.Close()
		if err == nil {
			t.Errorf("%s: an unreadable answer was reported as accepted", name)
		}
		if got := calls.Load(); got != 1 {
			t.Errorf("%s: offered %d times, want 1", name, got)
		}
	}
}

// The status travels with the error so the caller can tell the two apart, and
// that is the only thing separating them: both are ErrServerProtocol.
func TestPostReportsTheStatusItWasAnswered(t *testing.T) {
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte("{"))
	})
	defer server.Close()
	var response protocol.AcceptResponse
	status, err := agent.post(context.Background(), "/v1/agents/me/logs",
		protocol.AcceptResponse{SchemaVersion: protocol.Version}, &response, false)
	if err == nil {
		t.Fatal("a truncated answer decoded")
	}
	if status != http.StatusOK {
		t.Fatalf("status = %d, want %d", status, http.StatusOK)
	}
	if retryableEvidence(status) {
		t.Fatal("an answered call was classed as silence")
	}
}

// A call the plane genuinely never answered still reports status 0, and that
// is still the case worth repeating. This is the half the change must not move.
func TestACallThePlaneNeverAnsweredStillReportsSilence(t *testing.T) {
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {})
	server.Close()
	var response protocol.AcceptResponse
	status, err := agent.post(context.Background(), "/v1/agents/me/logs",
		protocol.AcceptResponse{SchemaVersion: protocol.Version}, &response, false)
	if err == nil {
		t.Fatal("a dead plane answered")
	}
	if status != 0 {
		t.Fatalf("status = %d, want 0", status)
	}
	if !retryableEvidence(status) {
		t.Fatal("silence was classed as a decision")
	}
}

// An oversized answer is the same shape of protocol break and takes the same
// route: answered, refused, not repeated.
func TestAnOversizedAnswerIsNotOfferedAgain(t *testing.T) {
	assignment := testAssignment()
	var calls atomic.Int64
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		filler := strings.Repeat("a", protocol.MaxJSONBytes+1)
		payload, _ := json.Marshal(map[string]string{"schema_version": filler})
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Content-Length", strconv.Itoa(len(payload)))
		_, _ = w.Write(payload)
	})
	defer server.Close()
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
	if _, err := uploader.write("format-check", "stdout", []byte("x")); err == nil {
		t.Fatal("an oversized answer was reported as accepted")
	}
	if got := calls.Load(); got != 1 {
		t.Fatalf("offered %d times, want 1", got)
	}
}

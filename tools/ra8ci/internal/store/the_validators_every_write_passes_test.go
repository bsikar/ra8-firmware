// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The store's small validators are the ones every write passes through, so a
// hole in one of them is a hole in every caller. None of them touches a
// database, and they were all uncovered.

// A fenced assignment gets another attempt only under the definition the run
// was planned against. A task whose definition moved since then is not
// retried under the new one, which is what binding the catalog digest does.
func TestARetryIsBoundToTheDefinitionTheRunWasPlannedAgainst(t *testing.T) {
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	digest := definitions.Digest()

	name := definitions.Names()[0]
	definition, found := definitions.Task(name)
	if !found {
		t.Fatalf("the catalog named %q and then did not know it", name)
	}
	limit := definition.Retry.MaxAttempts
	if limit < 1 {
		t.Fatalf("%q allows %d attempts, so nothing can ever run", name, limit)
	}

	if !retryable(definitions, name, digest, limit-1) {
		t.Fatalf("%q was refused a retry below its own limit of %d", name, limit)
	}
	if retryable(definitions, name, digest, limit) {
		t.Fatalf("%q was retried at its limit of %d", name, limit)
	}
	if retryable(definitions, name, digest, limit+1) {
		t.Fatalf("%q was retried past its limit of %d", name, limit)
	}

	for label, moved := range map[string]string{
		"no digest at all":     "",
		"a digest by eye":      "the-catalog",
		"a shouted digest":     strings.ToUpper(digest),
		"a digest one short":   digest[:len(digest)-1],
		"somebody else's plan": strings.Repeat("9", len(digest)),
	} {
		if retryable(definitions, name, moved, 0) {
			t.Fatalf("a retry was allowed under %s", label)
		}
	}

	for _, unknown := range []string{"", "not-a-task", strings.ToUpper(name)} {
		if retryable(definitions, unknown, digest, 0) {
			t.Fatalf("a retry was allowed for task %q", unknown)
		}
	}
}

// A task state is an outcome state exactly when the machine says so, and the
// ended_at constraint in the schema is written over that same set. Deriving
// the answer from the machine rather than a list here is the point: a new
// terminal state cannot be added without this agreeing.
func TestTerminalTaskStateIsTheMachine(t *testing.T) {
	// A task state is an outcome exactly when the machine gives it nowhere
	// left to go, so the answer is derived from the edges rather than from a
	// list kept beside them: a new outcome state cannot be added without
	// this agreeing, and a state that still has an edge cannot be one.
	for _, state := range taskMachine.states() {
		stillMoving := len(taskMachine.edges[state]) > 0
		if TerminalTaskState(state) == stillMoving {
			t.Fatalf("task state %q: terminal=%v while it has %d edges out",
				state, TerminalTaskState(state), len(taskMachine.edges[state]))
		}
		if TerminalTaskState(state) && CheckTaskTransition(state, "running") == nil {
			t.Fatalf("terminal task state %q still allows an edge back to running", state)
		}
	}

	// The two states a task can still be moved from, named so that turning one
	// of them into an outcome has to be a deliberate edit here too.
	for _, moving := range []string{"scheduled", "running"} {
		if TerminalTaskState(moving) {
			t.Fatalf("%q was taken as an outcome state", moving)
		}
	}
	for _, outcome := range []string{"succeeded", "failed", "timed_out", "cancelled", "preempted", "lost", "skipped"} {
		if !TerminalTaskState(outcome) {
			t.Fatalf("%q was not taken as an outcome state", outcome)
		}
	}

	for _, invented := range []string{"", "done", "SUCCEEDED", "terminal", "succeeded "} {
		if TerminalTaskState(invented) {
			t.Fatalf("%q was taken as a terminal task state", invented)
		}
	}
}

// A scale-set message is durable before it is acknowledged, so its identity
// has to be worth storing: three identifiers with their own bounds, and a
// payload that is a JSON object rather than any old JSON.
func TestAScaleSetMessageMustBeWorthStoring(t *testing.T) {
	whole := GitHubMessage{
		ScaleSetID: "scale-set-1", SessionID: "session-1", MessageID: "message-1",
		Payload: json.RawMessage(`{"jobs":1}`),
	}
	if !validMessage(whole) {
		t.Fatal("a whole message was refused")
	}

	atTheBounds := whole
	atTheBounds.ScaleSetID = strings.Repeat("s", 128)
	atTheBounds.SessionID = strings.Repeat("e", 256)
	atTheBounds.MessageID = strings.Repeat("m", 256)
	if !validMessage(atTheBounds) {
		t.Fatal("a message exactly at its bounds was refused")
	}

	for label, bend := range map[string]func(*GitHubMessage){
		"no scale set":             func(m *GitHubMessage) { m.ScaleSetID = "" },
		"no session":               func(m *GitHubMessage) { m.SessionID = "" },
		"no message ID":            func(m *GitHubMessage) { m.MessageID = "" },
		"a scale set one over":     func(m *GitHubMessage) { m.ScaleSetID = strings.Repeat("s", 129) },
		"a session one over":       func(m *GitHubMessage) { m.SessionID = strings.Repeat("e", 257) },
		"a message ID one over":    func(m *GitHubMessage) { m.MessageID = strings.Repeat("m", 257) },
		"a payload that is a list": func(m *GitHubMessage) { m.Payload = json.RawMessage(`[1,2]`) },
		"a payload that is a word": func(m *GitHubMessage) { m.Payload = json.RawMessage(`"jobs"`) },
		"a payload that is null":   func(m *GitHubMessage) { m.Payload = json.RawMessage(`null`) },
		"a payload half written":   func(m *GitHubMessage) { m.Payload = json.RawMessage(`{"jobs":`) },
	} {
		message := whole
		bend(&message)
		if validMessage(message) {
			t.Fatalf("a message with %s was stored", label)
		}
	}

	// An absent payload is the one shape that is filled in rather than
	// refused, so a message carrying no body is still storable.
	empty := whole
	empty.Payload = nil
	if !validMessage(empty) {
		t.Fatal("a message with no payload was refused")
	}
}

// validObject is the shared answer to "is this a JSON object": an absent one
// becomes an empty object, anything that is not an object is refused, and a
// real one is handed back unchanged rather than re-encoded.
func TestAJSONObjectIsTakenUnchangedOrRefused(t *testing.T) {
	filled, ok := validObject(nil)
	if !ok || string(filled) != "{}" {
		t.Fatalf("an absent object became %q, ok=%v", filled, ok)
	}
	if filled, ok = validObject(json.RawMessage{}); !ok || string(filled) != "{}" {
		t.Fatalf("an empty object became %q, ok=%v", filled, ok)
	}

	spaced := json.RawMessage(`{ "a" : 1 }`)
	kept, ok := validObject(spaced)
	if !ok || string(kept) != string(spaced) {
		t.Fatalf("an object was rewritten to %q", kept)
	}

	for _, notAnObject := range []string{`[]`, `1`, `"a"`, `true`, `null`, `{`, `{"a":}`, `nope`} {
		if _, ok := validObject(json.RawMessage(notAnObject)); ok {
			t.Fatalf("%q was taken as an object", notAnObject)
		}
	}
}

func aStartingAttempt() StartAttemptInput {
	return StartAttemptInput{
		TaskID: runnerVMTestEvidence, ActorID: "agent-1", Engine: "board-agent",
		Host: "lab-1", HostCores: 4, HostRAMBytes: 8 << 30, HostLoad: 0.5,
		HostFacts: json.RawMessage(`{"kernel":"6.6"}`),
	}
}

// An attempt is refused when its identifiers, engine or reported host shape
// could not have come from a healthy agent. The agent ID is the one that is
// optional: absent is fine, present and malformed is not.
func TestAnAttemptMustLookLikeAHealthyAgent(t *testing.T) {
	if !validHostFacts(aStartingAttempt()) {
		t.Fatal("a healthy attempt was refused")
	}

	withAgent := aStartingAttempt()
	withAgent.AgentID = runnerVMTestApproval
	if !validHostFacts(withAgent) {
		t.Fatal("an attempt naming a real agent was refused")
	}

	atTheFloor := aStartingAttempt()
	atTheFloor.HostCores = 1
	atTheFloor.HostRAMBytes = 1
	atTheFloor.HostLoad = 0
	atTheFloor.Engine = strings.Repeat("e", 64)
	if !validHostFacts(atTheFloor) {
		t.Fatal("an attempt exactly at its floor was refused")
	}

	for label, bend := range map[string]func(*StartAttemptInput){
		"no task":                func(in *StartAttemptInput) { in.TaskID = "" },
		"a task ID by eye":       func(in *StartAttemptInput) { in.TaskID = "task-1" },
		"an agent ID by eye":     func(in *StartAttemptInput) { in.AgentID = "agent-1" },
		"no actor":               func(in *StartAttemptInput) { in.ActorID = "" },
		"no engine":              func(in *StartAttemptInput) { in.Engine = "" },
		"an engine one over":     func(in *StartAttemptInput) { in.Engine = strings.Repeat("e", 65) },
		"no cores":               func(in *StartAttemptInput) { in.HostCores = 0 },
		"cores below zero":       func(in *StartAttemptInput) { in.HostCores = -1 },
		"no memory":              func(in *StartAttemptInput) { in.HostRAMBytes = 0 },
		"memory below zero":      func(in *StartAttemptInput) { in.HostRAMBytes = -1 },
		"a load below zero":      func(in *StartAttemptInput) { in.HostLoad = -0.1 },
		"a load that is not one": func(in *StartAttemptInput) { in.HostLoad = math.NaN() },
		"an endless load":        func(in *StartAttemptInput) { in.HostLoad = math.Inf(1) },
		"facts that are a list":  func(in *StartAttemptInput) { in.HostFacts = json.RawMessage(`[1]`) },
	} {
		attempt := aStartingAttempt()
		bend(&attempt)
		if validHostFacts(attempt) {
			t.Fatalf("an attempt with %s was accepted", label)
		}
	}
}

// A load average is a real, non-negative number. Negative infinity is the
// arm a lone IsInf(load, 1) check would miss, and it is refused by the sign
// test before the infinity test is reached.
func TestALoadAverageIsRealAndNonNegative(t *testing.T) {
	for _, load := range []float64{0, 0.01, 1, 64, math.MaxFloat64} {
		if !validLoad(load) {
			t.Fatalf("load %v was refused", load)
		}
	}
	for _, load := range []float64{-0.0001, -1, math.NaN(), math.Inf(1), math.Inf(-1)} {
		if validLoad(load) {
			t.Fatalf("load %v was accepted", load)
		}
	}
	// Negative zero is zero, so it is a real reading rather than a negative one.
	if !validLoad(math.Copysign(0, -1)) {
		t.Fatal("negative zero was refused as a load")
	}
}

// An empty string is written as SQL NULL rather than as an empty string, so a
// column that means "nothing was recorded" never fills with blanks that would
// then satisfy a NOT NULL read.
func TestAnEmptyStringIsWrittenAsNull(t *testing.T) {
	if got := nullable(""); got != nil {
		t.Fatalf("an empty string was written as %#v", got)
	}
	for _, kept := range []string{" ", "0", "null", "lab-1"} {
		got, ok := nullable(kept).(string)
		if !ok || got != kept {
			t.Fatalf("%q was written as %#v", kept, nullable(kept))
		}
	}
}

// A board actor is issued only after a verified peer is mapped to a grant,
// and its fields are private so an HTTP body cannot manufacture identity. The
// reader is the only way out, and a zero actor reads as no identity at all
// rather than as some default one.
func TestABoardActorOnlyReadsBackWhatItWasIssued(t *testing.T) {
	if got := (BoardActor{}).ID(); got != "" {
		t.Fatalf("a zero actor claimed the identity %q", got)
	}
	issued := BoardActor{id: runnerVMTestEvidence, kind: "agent", role: "board", boardID: "ek~ra8d2"}
	if got := issued.ID(); got != runnerVMTestEvidence {
		t.Fatalf("an issued actor read back %q", got)
	}
}

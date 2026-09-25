// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A shadow run exists to be compared against Actions, not to move a pull
// request. Branch protection reads success, neutral and skipped as satisfying a
// required check, so a shadow run reporting anything outside that set would
// block a merge over a publisher that is still being evaluated.
func TestAShadowRunNeverCarriesABlockingConclusion(t *testing.T) {
	for state := range observedConclusions {
		run, err := NewTaskCheckRun(ModeShadow, "build", "0123456789abcdef0123456789abcdef01234567", state)
		if err != nil {
			t.Fatalf("shadow run for %s: %v", state, err)
		}
		if run.Conclusion != shadowConclusion {
			t.Fatalf("shadow run for %s reported %q, want %q", state, run.Conclusion, shadowConclusion)
		}
		if run.Blocking() {
			t.Fatalf("shadow run for %s is blocking", state)
		}
		if run.Status != "completed" {
			t.Fatalf("shadow run for %s has status %q", state, run.Status)
		}
	}
}

// Reporting neutral is only acceptable because the observed conclusion is still
// recoverable from the run itself; otherwise shadow mode would publish runs
// nothing can be compared against.
func TestAShadowRunStillCarriesTheObservedConclusion(t *testing.T) {
	run, err := NewTaskCheckRun(ModeShadow, "build", "0123456789abcdef0123456789abcdef01234567", "failed")
	if err != nil {
		t.Fatalf("shadow run: %v", err)
	}
	if run.Observed != "failure" {
		t.Fatalf("observed %q, want failure", run.Observed)
	}
	if !strings.Contains(run.Title, "failure") || !strings.Contains(run.Title, "build") {
		t.Fatalf("title %q names neither the task nor the observed conclusion", run.Title)
	}
	if run.Conclusion == run.Observed {
		t.Fatal("shadow run posted the observed conclusion")
	}
}

// An authoritative run is the merge gate, so it says what the plane saw.
func TestAnAuthoritativeRunReportsWhatWasObserved(t *testing.T) {
	for state, want := range observedConclusions {
		run, err := NewTaskCheckRun(ModeAuthoritative, "build", "0123456789abcdef0123456789abcdef01234567", state)
		if err != nil {
			t.Fatalf("authoritative run for %s: %v", state, err)
		}
		if run.Conclusion != want || run.Observed != want {
			t.Fatalf("authoritative run for %s reported %q/%q, want %q", state, run.Conclusion, run.Observed, want)
		}
		if run.Blocking() == nonBlockingConclusions[want] {
			t.Fatalf("%s: conclusion %q is both blocking and not", state, want)
		}
	}
}

// A lost task is the one outcome where silence must not read as a pass: the
// plane never learned what the attempt did, so the gate asks for a human.
func TestALostTaskDoesNotSatisfyAGate(t *testing.T) {
	run, err := NewTaskCheckRun(ModeAuthoritative, "build", "0123456789abcdef0123456789abcdef01234567", "lost")
	if err != nil {
		t.Fatalf("lost run: %v", err)
	}
	if !run.Blocking() {
		t.Fatalf("a lost task reported %q, which satisfies a required check", run.Conclusion)
	}
}

// The two namespaces cannot collide, so a required check name configured for
// the authoritative publisher can never be satisfied by a shadow run. Pinned
// over every name the embedded catalog carries, not over one example.
func TestNoShadowNameIsAnAuthoritativeName(t *testing.T) {
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	names := definitions.Names()
	if len(names) == 0 {
		t.Fatal("embedded catalog is empty")
	}
	authoritative := make(map[string]string, len(names))
	for _, name := range names {
		published, err := CheckRunName(ModeAuthoritative, name)
		if err != nil {
			t.Fatalf("authoritative name for %s: %v", name, err)
		}
		if previous, clash := authoritative[published]; clash {
			t.Fatalf("%s and %s publish the same name %q", previous, name, published)
		}
		authoritative[published] = name
	}
	for _, name := range names {
		shadow, err := CheckRunName(ModeShadow, name)
		if err != nil {
			t.Fatalf("shadow name for %s: %v", name, err)
		}
		if owner, clash := authoritative[shadow]; clash {
			t.Fatalf("shadow name for %s equals the required name of %s", name, owner)
		}
	}
}

// The name rule is restated in this package, so it is pinned against the
// catalog it has to agree with: every reviewed definition must be nameable.
func TestCheckRunTaskRuleMatchesTheEmbeddedCatalog(t *testing.T) {
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	for _, name := range definitions.Names() {
		if !validCheckRunTask(name) {
			t.Fatalf("catalog task %q cannot be published as a check run", name)
		}
	}
}

// Every state this file maps has to be an outcome of the task machine in
// internal/store. The reverse direction, a terminal state added to the machine
// with no mapping here, cannot be pinned from outside the store package: the
// machine's state set is unexported and there is no exported enumerator, so
// ObservedConclusion refuses an unmapped state at runtime instead.
func TestEveryMappedStateIsTerminalInTheTaskMachine(t *testing.T) {
	for state := range observedConclusions {
		if !store.TerminalTaskState(state) {
			t.Fatalf("mapped state %q is not an outcome of the task machine", state)
		}
	}
}

// A task that has not ended has no conclusion, and saying so is different from
// saying the state does not exist.
func TestAnUnfinishedTaskIsRefusedAsNotAnOutcome(t *testing.T) {
	for _, state := range []string{"scheduled", "running"} {
		if store.TerminalTaskState(state) {
			t.Fatalf("%q is terminal in the task machine, the fixture is stale", state)
		}
		if _, err := ObservedConclusion(state); !errors.Is(err, ErrTaskStateNotTerminal) {
			t.Fatalf("ObservedConclusion(%q) = %v, want ErrTaskStateNotTerminal", state, err)
		}
	}
}

// An unknown state is refused rather than mapped to a verdict. Reading an
// unrecognised state as failure would fail pull requests over a schema change.
func TestAnUnknownStateIsRefusedNotGuessed(t *testing.T) {
	for _, state := range []string{"", "SUCCEEDED", "success", "done", "requeued"} {
		if _, err := ObservedConclusion(state); !errors.Is(err, ErrUnknownTaskState) {
			t.Fatalf("ObservedConclusion(%q) = %v, want ErrUnknownTaskState", state, err)
		}
	}
}

// Misuse is refused before anything is built, and each refusal names its own
// reason so a publisher can tell a bad task name from a bad commit.
func TestMisuseIsRefusedWithItsOwnReason(t *testing.T) {
	const sha = "0123456789abcdef0123456789abcdef01234567"
	for _, testCase := range []struct {
		name  string
		mode  CheckRunMode
		task  string
		sha   string
		state string
		want  error
	}{
		{name: "mode below the set", mode: CheckRunMode(-1), task: "build", sha: sha, state: "succeeded", want: ErrInvalidCheckRunMode},
		{name: "mode above the set", mode: CheckRunMode(2), task: "build", sha: sha, state: "succeeded", want: ErrInvalidCheckRunMode},
		{name: "empty task", mode: ModeShadow, task: "", sha: sha, state: "succeeded", want: ErrInvalidCheckRunTask},
		{name: "upper case task", mode: ModeShadow, task: "Build", sha: sha, state: "succeeded", want: ErrInvalidCheckRunTask},
		{name: "task with a separator", mode: ModeShadow, task: "ra8ci / build", sha: sha, state: "succeeded", want: ErrInvalidCheckRunTask},
		{name: "task with a space", mode: ModeShadow, task: "build all", sha: sha, state: "succeeded", want: ErrInvalidCheckRunTask},
		{name: "short sha", mode: ModeShadow, task: "build", sha: "0123456", state: "succeeded", want: ErrInvalidCheckRunSHA},
		{name: "non hex sha", mode: ModeShadow, task: "build", sha: strings.Repeat("g", 40), state: "succeeded", want: ErrInvalidCheckRunSHA},
		{name: "empty sha", mode: ModeShadow, task: "build", sha: "", state: "succeeded", want: ErrInvalidCheckRunSHA},
		{name: "unfinished task", mode: ModeShadow, task: "build", sha: sha, state: "running", want: ErrTaskStateNotTerminal},
		{name: "unknown state", mode: ModeShadow, task: "build", sha: sha, state: "exploded", want: ErrUnknownTaskState},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			run, err := NewTaskCheckRun(testCase.mode, testCase.task, testCase.sha, testCase.state)
			if !errors.Is(err, testCase.want) {
				t.Fatalf("error %v, want %v", err, testCase.want)
			}
			if run != (TaskCheckRun{}) {
				t.Fatalf("refused call returned %+v", run)
			}
		})
	}
}

// A head SHA is recorded lower case whatever the caller passed, so two runs on
// one commit carry one spelling of it.
func TestHeadSHAIsRecordedLowerCase(t *testing.T) {
	run, err := NewTaskCheckRun(ModeShadow, "build", "0123456789ABCDEF0123456789ABCDEF01234567", "succeeded")
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if run.HeadSHA != "0123456789abcdef0123456789abcdef01234567" {
		t.Fatalf("head SHA %q", run.HeadSHA)
	}
}

// The mode names itself in errors and logs, including a value outside the set.
func TestModeNamesItself(t *testing.T) {
	if ModeShadow.String() != "shadow" || ModeAuthoritative.String() != "authoritative" {
		t.Fatalf("modes name themselves %q and %q", ModeShadow, ModeAuthoritative)
	}
	if got := CheckRunMode(7).String(); got != "CheckRunMode(7)" {
		t.Fatalf("out of set mode names itself %q", got)
	}
}

// A title past GitHub's ceiling is cut by character, never by byte. A byte cut
// can halve a multi-byte character, and the broken text GitHub then renders is
// the one line a reviewer reads the run's conclusion from.
func TestALongTitleIsCutByCharacterAndStaysReadable(t *testing.T) {
	long := strings.Repeat("\u756e", 400)
	cut := boundCheckRunTitle(long)
	if utf8.RuneCountInString(cut) != maxCheckRunTitle {
		t.Fatalf("cut title is %d characters, want %d", utf8.RuneCountInString(cut), maxCheckRunTitle)
	}
	if !utf8.ValidString(cut) || strings.ContainsRune(cut, utf8.RuneError) {
		t.Fatalf("cut title is not readable text: %q", cut)
	}
	if short := "ra8ci shadow"; boundCheckRunTitle(short) != short {
		t.Fatal("a title inside the ceiling was altered")
	}
	exact := strings.Repeat("t", maxCheckRunTitle)
	if boundCheckRunTitle(exact) != exact {
		t.Fatal("a title of exactly the ceiling was cut")
	}
}

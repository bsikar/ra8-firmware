// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"strings"
)

// #1481 publishes one check run per catalog task through the GitHub App, with
// branch protection eventually requiring those names. The issue is explicit
// that this runs in shadow mode first, with conclusions compared against
// Actions before any required check moves, so this file states the two rules a
// shadow publisher has to get right before anything is posted: what a check run
// is called, and what conclusion it is allowed to carry.
//
// The safety argument is about what GitHub does with a conclusion, not about
// what ra8ci intends. Branch protection reads `success`, `neutral` and
// `skipped` as satisfying a required check and `failure`, `timed_out`,
// `cancelled` and `action_required` as not satisfying it. A shadow run must
// therefore never carry one of the second set: while we are still learning
// whether ra8ci agrees with Actions, a disagreement must cost an operator a
// comparison, never a blocked pull request.

const (
	// checkRunNamespace prefixes a check run this plane is willing to have
	// required. It is the name branch protection would be pointed at.
	checkRunNamespace = "ra8ci"
	// shadowCheckRunNamespace prefixes a check run published only for
	// comparison against Actions. It is deliberately a different name so a
	// required check configured today cannot be satisfied by a shadow run.
	shadowCheckRunNamespace = "ra8ci-shadow"
	// checkRunNameSeparator matches the "context / job" shape GitHub renders
	// for its own check runs.
	checkRunNameSeparator = " / "
	// maxCheckRunTitle bounds the observed-conclusion title a shadow run
	// carries; GitHub's own limit is larger and this is only ever a short
	// sentence.
	maxCheckRunTitle = 255
)

// CheckRunMode says whether a check run is published for comparison or for the
// merge gate. There are exactly two, and the zero value is the safe one.
type CheckRunMode int

const (
	// ModeShadow publishes under the shadow namespace and reports neutral
	// whatever the task did, so the run cannot move a pull request.
	ModeShadow CheckRunMode = iota
	// ModeAuthoritative publishes under the required namespace and reports
	// the conclusion the plane observed.
	ModeAuthoritative
)

// String names the mode for errors and logs.
func (m CheckRunMode) String() string {
	switch m {
	case ModeShadow:
		return "shadow"
	case ModeAuthoritative:
		return "authoritative"
	default:
		return fmt.Sprintf("CheckRunMode(%d)", int(m))
	}
}

var (
	// ErrInvalidCheckRunMode is returned for a mode outside the two above.
	ErrInvalidCheckRunMode = errors.New("unknown check run mode")
	// ErrInvalidCheckRunTask is returned for a task name outside the
	// catalog's name rule.
	ErrInvalidCheckRunTask = errors.New("invalid catalog task name for a check run")
	// ErrInvalidCheckRunSHA is returned for a head SHA that is not a
	// 40-character hexadecimal commit.
	ErrInvalidCheckRunSHA = errors.New("invalid head SHA for a check run")
	// ErrUnknownTaskState is returned for a task state this file has no
	// mapping for, rather than guessing one.
	ErrUnknownTaskState = errors.New("no check run conclusion for task state")
	// ErrTaskStateNotTerminal is returned when a conclusion is asked for a
	// task that has not ended.
	ErrTaskStateNotTerminal = errors.New("task state is not an outcome")
)

// observedConclusions maps every terminal state of the task machine in
// internal/store to the GitHub check run conclusion that states it honestly.
// TestEveryTerminalTaskStateHasAConclusion pins that the two sets agree, so a
// state added to the machine fails a test here instead of reaching a publisher
// with no mapping.
//
// Two of these are judgement rather than translation:
//
//   - preempted is neutral. A preempted task yielded its board and was never
//     judged, so there is no verdict to report; neutral says "ran, no opinion"
//     and does not hold up a merge for work that will be run again.
//   - lost is action_required. The plane never learned what the attempt did.
//     Silence is not a pass, and neutral would be read as one, so a lost task
//     asks for a human rather than quietly satisfying a gate.
var observedConclusions = map[string]string{
	"succeeded": "success",
	"failed":    "failure",
	"timed_out": "timed_out",
	"cancelled": "cancelled",
	"skipped":   "skipped",
	"preempted": "neutral",
	"lost":      "action_required",
}

// nonBlockingConclusions is the set branch protection treats as satisfying a
// required check. A shadow run must report one of these and does, by reporting
// the first of them.
var nonBlockingConclusions = map[string]bool{
	"success": true,
	"neutral": true,
	"skipped": true,
}

// shadowConclusion is what every completed shadow run reports, whatever the
// task did. The observed conclusion travels in the run's title instead.
const shadowConclusion = "neutral"

// CheckRunName returns the name a check run for this task is published under.
// The two namespaces never collide: an authoritative name continues with a
// space after "ra8ci" and a shadow name continues with a hyphen, so no task
// name can produce one from the other. TestNoShadowNameIsAnAuthoritativeName
// pins it over the whole embedded catalog.
func CheckRunName(mode CheckRunMode, task string) (string, error) {
	if mode != ModeShadow && mode != ModeAuthoritative {
		return "", fmt.Errorf("%w: %s", ErrInvalidCheckRunMode, mode)
	}
	if !validCheckRunTask(task) {
		return "", fmt.Errorf("%w: %q", ErrInvalidCheckRunTask, task)
	}
	namespace := checkRunNamespace
	if mode == ModeShadow {
		namespace = shadowCheckRunNamespace
	}
	return namespace + checkRunNameSeparator + task, nil
}

// ObservedConclusion reports the conclusion the plane actually observed for a
// terminal task state, whatever mode a publisher is running in. A comparison
// against Actions is made against this, not against what was posted.
func ObservedConclusion(state string) (string, error) {
	conclusion, mapped := observedConclusions[state]
	if mapped {
		return conclusion, nil
	}
	if knownTaskState(state) {
		return "", fmt.Errorf("%w: %q", ErrTaskStateNotTerminal, state)
	}
	return "", fmt.Errorf("%w: %q", ErrUnknownTaskState, state)
}

// TaskCheckRun is one completed check run, ready to be posted. It carries only
// the fields the shadow comparison needs; the publisher that posts it is a
// later slice.
type TaskCheckRun struct {
	Name       string
	HeadSHA    string
	Status     string
	Conclusion string
	Title      string
	Mode       CheckRunMode
	// Observed is the conclusion the plane saw. In authoritative mode it is
	// the same as Conclusion; in shadow mode Conclusion is neutral and this
	// is what the comparison against Actions uses.
	Observed string
}

// NewTaskCheckRun builds the completed check run for one task outcome.
//
// In shadow mode the posted conclusion is neutral whatever the task did, so the
// run cannot fail a pull request or satisfy a gate it was never meant to reach,
// and the observed conclusion is stated in the title so the comparison the
// issue asks for can be made from the check run itself.
func NewTaskCheckRun(mode CheckRunMode, task, headSHA, state string) (TaskCheckRun, error) {
	name, err := CheckRunName(mode, task)
	if err != nil {
		return TaskCheckRun{}, err
	}
	if !validCommitSHA(headSHA) {
		return TaskCheckRun{}, fmt.Errorf("%w: %q", ErrInvalidCheckRunSHA, headSHA)
	}
	observed, err := ObservedConclusion(state)
	if err != nil {
		return TaskCheckRun{}, err
	}
	run := TaskCheckRun{
		Name:       name,
		HeadSHA:    strings.ToLower(headSHA),
		Status:     "completed",
		Conclusion: observed,
		Title:      fmt.Sprintf("%s: %s", task, observed),
		Mode:       mode,
		Observed:   observed,
	}
	if mode == ModeShadow {
		run.Conclusion = shadowConclusion
		run.Title = fmt.Sprintf("shadow: %s would report %s", task, observed)
	}
	if len(run.Title) > maxCheckRunTitle {
		run.Title = run.Title[:maxCheckRunTitle]
	}
	return run, nil
}

// Blocking reports whether the conclusion this run carries is one branch
// protection refuses to merge over. A shadow run is never blocking.
func (r TaskCheckRun) Blocking() bool { return !nonBlockingConclusions[r.Conclusion] }

// validCheckRunTask applies the catalog's own name rule. It is restated rather
// than imported because internal/catalog does not export it, and a check run
// name that drifted from the catalog's rule would be a name no task answers to.
// TestCheckRunTaskRuleMatchesTheEmbeddedCatalog pins the two against every
// reviewed definition.
func validCheckRunTask(value string) bool {
	if value == "" || len(value) > 100 {
		return false
	}
	for _, char := range value {
		if (char < 'a' || char > 'z') && (char < '0' || char > '9') && char != '-' {
			return false
		}
	}
	return true
}

// knownTaskState reports whether the task machine has this state at all, so a
// state that exists but has not ended is refused differently from a state that
// does not exist.
func knownTaskState(state string) bool {
	switch state {
	case "scheduled", "running":
		return true
	default:
		_, terminal := observedConclusions[state]
		return terminal
	}
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"sort"
	"strings"
)

// #1481's end state is branch protection requiring the plane's check run names.
// Everything landed so far builds, grades and posts those runs; nothing said
// which contexts branch protection should be asked to require, or what to do
// with the ones it requires today. This file answers that as a plan the
// operator reads before anything is changed, because moving a required check is
// the step that can hold every pull request in the repository.
//
// The plan is computed, never applied. Nothing here speaks to GitHub.

var (
	// ErrRequiredCheckTaskInvalid is returned for a task name outside the
	// catalog's name rule.
	ErrRequiredCheckTaskInvalid = errors.New("invalid catalog task name for a required check")
	// ErrRequiredCheckContextInvalid is returned for a currently-required
	// context that is empty or carries surrounding whitespace, neither of
	// which is a name GitHub posts.
	ErrRequiredCheckContextInvalid = errors.New("invalid required status check context")
	// ErrRequiredCheckSetAmbiguous is returned when the same task or the
	// same context is named twice, rather than silently keeping one.
	ErrRequiredCheckSetAmbiguous = errors.New("required check set names the same thing twice")
	// ErrNoRequiredCheckTasks is returned for an empty task set. A plan
	// built from no tasks proposes tearing the whole gate down, which is
	// what a failed catalog load looks like.
	ErrNoRequiredCheckTasks = errors.New("no catalog tasks to require")
)

// RequiredCheckPlan is what branch protection's required-context list would
// become, split by what the operator has to decide about each name.
type RequiredCheckPlan struct {
	// Mode is the mode the plan was computed for. A shadow plan never adds.
	Mode CheckRunMode
	// Add names contexts to require that are not required today.
	Add []string
	// Remove names required contexts this plane owns that no longer belong:
	// a shadow name, a name no reviewed task answers to, or every one of
	// them while the deployment is in shadow mode.
	Remove []string
	// Keep names required contexts that are already right.
	Keep []string
	// Foreign names required contexts this plane does not own, reported so
	// the operator sees the whole gate and never so the plan touches them.
	Foreign []string
}

// NoChange reports whether the gate already matches this plan.
func (p RequiredCheckPlan) NoChange() bool { return len(p.Add) == 0 && len(p.Remove) == 0 }

// PlanRequiredChecks works out what branch protection should require for these
// catalog tasks, given what it requires today.
//
// A shadow deployment plans no additions at all. A shadow run reports neutral
// whatever the task did (check_runs.go), and branch protection reads neutral as
// satisfying a required check, so requiring a shadow name buys a gate that can
// never fail: worse than no gate, because it reads as protection. #1481 holds
// the required-check move until conclusions have been compared against Actions,
// and this is that hold expressed as something a deployment cannot get wrong by
// pointing branch protection at whatever it happens to be publishing.
func PlanRequiredChecks(mode CheckRunMode, tasks []string, currentlyRequired []string) (RequiredCheckPlan, error) {
	if mode != ModeShadow && mode != ModeAuthoritative {
		return RequiredCheckPlan{}, fmt.Errorf("%w: %s", ErrInvalidCheckRunMode, mode)
	}
	if len(tasks) == 0 {
		return RequiredCheckPlan{}, ErrNoRequiredCheckTasks
	}

	wanted := make(map[string]string, len(tasks))
	seenTask := make(map[string]bool, len(tasks))
	for _, task := range tasks {
		if !validCheckRunTask(task) {
			return RequiredCheckPlan{}, fmt.Errorf("%w: %q", ErrRequiredCheckTaskInvalid, task)
		}
		if seenTask[task] {
			return RequiredCheckPlan{}, fmt.Errorf("%w: task %q", ErrRequiredCheckSetAmbiguous, task)
		}
		seenTask[task] = true
		name, err := CheckRunName(ModeAuthoritative, task)
		if err != nil {
			return RequiredCheckPlan{}, err
		}
		wanted[name] = task
	}

	plan := RequiredCheckPlan{Mode: mode}
	required := make(map[string]bool, len(currentlyRequired))
	for _, context := range currentlyRequired {
		if context == "" || strings.TrimSpace(context) != context {
			return RequiredCheckPlan{}, fmt.Errorf("%w: %q", ErrRequiredCheckContextInvalid, context)
		}
		if required[context] {
			return RequiredCheckPlan{}, fmt.Errorf("%w: context %q", ErrRequiredCheckSetAmbiguous, context)
		}
		required[context] = true

		switch owner := ownerOfContext(context); owner {
		case contextShadow:
			// A shadow name can only ever report neutral, so a gate
			// pointed at one is satisfied by a task that failed.
			// Removed in either mode.
			plan.Remove = append(plan.Remove, context)
		case contextAuthoritative:
			switch {
			case mode == ModeShadow:
				// This deployment publishes no authoritative run,
				// so the context would never be answered and every
				// pull request would wait on it forever.
				plan.Remove = append(plan.Remove, context)
			case wanted[context] != "":
				plan.Keep = append(plan.Keep, context)
			default:
				// Nothing will ever post it: no reviewed task
				// carries the name.
				plan.Remove = append(plan.Remove, context)
			}
		default:
			plan.Foreign = append(plan.Foreign, context)
		}
	}

	if mode == ModeAuthoritative {
		for name := range wanted {
			if !required[name] {
				plan.Add = append(plan.Add, name)
			}
		}
	}

	sort.Strings(plan.Add)
	sort.Strings(plan.Remove)
	sort.Strings(plan.Keep)
	sort.Strings(plan.Foreign)
	return plan, nil
}

// contextOwner says whose name a required context is.
type contextOwner int

const (
	// contextForeign is a context this plane does not publish: an Actions
	// job, another App's check run, anything else on the gate.
	contextForeign contextOwner = iota
	// contextAuthoritative is a name this plane publishes to be required.
	contextAuthoritative
	// contextShadow is a name this plane publishes only for comparison.
	contextShadow
)

// ownerOfContext classifies one required context by namespace. The two
// namespaces cannot collide (check_runs.go), so the shadow prefix is tested
// first and an authoritative name can never be read as a shadow one.
func ownerOfContext(context string) contextOwner {
	if strings.HasPrefix(context, shadowCheckRunNamespace+checkRunNameSeparator) {
		return contextShadow
	}
	if strings.HasPrefix(context, checkRunNamespace+checkRunNameSeparator) {
		return contextAuthoritative
	}
	return contextForeign
}

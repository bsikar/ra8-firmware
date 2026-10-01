// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"fmt"
	"sort"
	"strings"
)

// maxSubmittedTasks bounds one submission, matching the older word bound that
// `run submit` applied before any word could be an argument.
const maxSubmittedTasks = 100

// submittedTask is one task named on a `run submit` command line together with
// the argument words that belong to it.
type submittedTask struct {
	Name  string
	Words []string
}

// splitSubmitTasks reads `TASK [name=value...] [TASK [name=value...]]...`.
//
// A word containing '=' is an argument and attaches to the task most recently
// named; every other word names a new task. That split needs no separator and
// no qualified `task.name=value` form because a catalog task name is lowercase
// letters, digits and '-' only (catalog.validName), so a task name can never
// contain '=' and an argument word can never be mistaken for one.
//
// An argument before any task name is refused rather than attached to
// something later: a caller who typed the value first means it for a task the
// parser cannot know, and guessing would bind it to the wrong one.
func splitSubmitTasks(words []string) ([]submittedTask, error) {
	tasks := make([]submittedTask, 0, len(words))
	for _, word := range words {
		if strings.Contains(word, "=") {
			if len(tasks) == 0 {
				return nil, fmt.Errorf("argument %q names no task: arguments follow the task they belong to", word)
			}
			last := &tasks[len(tasks)-1]
			last.Words = append(last.Words, word)
			continue
		}
		if len(tasks) == maxSubmittedTasks {
			return nil, fmt.Errorf("at most %d tasks per submission", maxSubmittedTasks)
		}
		tasks = append(tasks, submittedTask{Name: word})
	}
	if len(tasks) == 0 {
		return nil, fmt.Errorf("no task named")
	}
	return tasks, nil
}

// submissionIdentity is what makes two entries in one submission the same
// work: the task, and the values it would run with.
//
// `run submit` has always refused the same task twice, and that refusal was
// about submitting one piece of work twice in a single run. With arguments
// that is no longer the same thing as naming a task twice: the same task with
// different values is different work, and the two entries already get
// distinct task keys. So the refusal now keys on the pair.
func submissionIdentity(name string, values map[string]string) string {
	if len(values) == 0 {
		return name
	}
	pairs := make([]string, 0, len(values))
	for valueName, value := range values {
		pairs = append(pairs, valueName+"="+value)
	}
	sort.Strings(pairs)
	return name + "\x00" + strings.Join(pairs, "\x00")
}

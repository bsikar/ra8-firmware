// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"fmt"
	"sort"
	"strings"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// maxCommandLineArguments bounds what one invocation may supply, so a mistyped
// shell glob is refused as a usage error rather than walked.
const maxCommandLineArguments = catalog.MaxPositionalArguments + catalog.MaxFlagArguments

// parseTaskArguments reads name=value words off the command line into the
// named values a reviewed schema binds against.
//
// name=value is the only accepted form. A bare word is refused rather than
// guessed at as the next positional, because a task's positionals are
// declared by name and a caller who miscounted them would otherwise bind a
// value to the wrong argument and never see it.
//
// The first '=' separates: a value may contain one, a name may not.
func parseTaskArguments(words []string) (map[string]string, error) {
	if len(words) > maxCommandLineArguments {
		return nil, fmt.Errorf("at most %d task arguments", maxCommandLineArguments)
	}
	values := make(map[string]string, len(words))
	for _, word := range words {
		name, value, found := strings.Cut(word, "=")
		if !found {
			return nil, fmt.Errorf("task arguments are name=value: %q", word)
		}
		if !catalog.ValidArgumentName(name) {
			return nil, fmt.Errorf("invalid argument name %q", name)
		}
		if _, repeated := values[name]; repeated {
			return nil, fmt.Errorf("argument %q supplied more than once", name)
		}
		if !catalog.ValidArgumentValue(value) {
			return nil, fmt.Errorf("invalid value for argument %q", name)
		}
		values[name] = value
	}
	return values, nil
}

// taskArgumentValues is what a command line supplies to one task.
//
// A task that declares no arguments keeps the older, plainer refusal: every
// task in the v1 catalog is in that case, so `ra8ci format-check extra` still
// says the task accepts no arguments rather than complaining about name=value
// shape for a word that was never going to be an argument.
func taskArgumentValues(task catalog.Task, words []string) (map[string]string, error) {
	if len(task.ArgsSchema.Positional) == 0 && len(task.ArgsSchema.Flags) == 0 {
		if err := task.ValidateArguments(words); err != nil {
			return nil, err
		}
		return nil, nil
	}
	values, err := parseTaskArguments(words)
	if err != nil {
		return nil, fmt.Errorf("%v (task %q accepts %s)", err, task.Name, argumentUsage(task))
	}
	return values, nil
}

// argumentUsage names what a task accepts, so a refusal tells the caller what
// to type instead of only what was wrong.
func argumentUsage(task catalog.Task) string {
	names := task.ArgsSchema.ArgumentNames()
	if len(names) == 0 {
		return "no arguments"
	}
	sorted := append([]string(nil), names...)
	sort.Strings(sorted)
	return strings.Join(sorted, ", ")
}

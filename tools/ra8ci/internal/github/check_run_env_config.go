// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
)

// shadow_correspondence.go says the task-to-job correspondence is operator
// data rather than a constant, because the workflow's job names are the
// workflow's to choose. Nothing read it. This file is the configuration path:
// where the correspondence is declared, and which mode a publisher built from
// it runs in.
//
// The correspondence is a file rather than a variable holding a list. The
// catalog carries eighty-odd tasks, so a correspondence that covers a
// meaningful part of it does not belong on a process environment line, and a
// file can be reviewed in a diff.

const (
	// EnvShadowCorrespondenceFile names the JSON file declaring which
	// Actions job covers which catalog task.
	EnvShadowCorrespondenceFile = "RA8CI_GITHUB_SHADOW_CORRESPONDENCE_FILE"
	// EnvCheckRunMode names the mode a check-run publisher runs in. Absent
	// means shadow.
	EnvCheckRunMode = "RA8CI_GITHUB_CHECK_RUN_MODE"

	// maxCorrespondenceFileBytes bounds the declaration this reads. A
	// correspondence covering every catalog task is a few kilobytes, so
	// this is room to spare and still a refusal rather than an unbounded
	// read of whatever the path points at.
	maxCorrespondenceFileBytes = 256 << 10
)

// ErrCheckRunConfigUnreadable is returned when the declared correspondence
// file cannot be read or does not hold a correspondence. The error names the
// path and what was wrong with it, never the file's contents.
var ErrCheckRunConfigUnreadable = errors.New("shadow correspondence file is unreadable")

// CheckRunEnvConfig is the reviewed check-run configuration for one process.
type CheckRunEnvConfig struct {
	// Mode is what a publisher built from this configuration may do.
	Mode CheckRunMode
	// Correspondence is the declared task-to-job map, already checked
	// against the catalog.
	Correspondence *ShadowCorrespondence
}

// LoadCheckRunConfigFromEnv reads the check-run configuration, returning
// enabled=false only when neither variable is set, the convention
// LoadSessionConfigFromEnv already follows.
//
// knownTasks is the catalog's name list, passed through to
// NewShadowCorrespondence so a declaration naming a task no reviewed
// definition carries is refused at startup rather than at the first
// comparison.
func LoadCheckRunConfigFromEnv(knownTasks []string) (config CheckRunEnvConfig, enabled bool, err error) {
	path, pathSet := os.LookupEnv(EnvShadowCorrespondenceFile)
	mode, modeSet := os.LookupEnv(EnvCheckRunMode)
	if !pathSet && !modeSet {
		return CheckRunEnvConfig{}, false, nil
	}

	// The mode is resolved before the file is opened, so a process asking
	// for a mode this build will not run refuses without having read
	// anything.
	config.Mode, err = parseCheckRunMode(mode, modeSet)
	if err != nil {
		return CheckRunEnvConfig{}, false, err
	}

	if !pathSet || strings.TrimSpace(path) == "" {
		return CheckRunEnvConfig{}, false, &EnvConfigError{
			Variable: EnvShadowCorrespondenceFile,
			Problem:  "required when check-run publishing is configured",
		}
	}
	pairs, err := readCorrespondenceFile(path)
	if err != nil {
		return CheckRunEnvConfig{}, false, err
	}
	correspondence, err := NewShadowCorrespondence(pairs, knownTasks)
	if err != nil {
		return CheckRunEnvConfig{}, false, fmt.Errorf("%s: %w", path, err)
	}
	config.Correspondence = correspondence
	return config, true, nil
}

// parseCheckRunMode resolves the mode an operator asked for.
//
// An absent variable is shadow, not an error and never authoritative. Shadow
// is the zero value of CheckRunMode for the same reason: #1481 holds the
// required-check move until conclusions have been compared, so a deployment
// that forgets to say which mode it wants must land in the one that cannot
// move a pull request. Authoritative is spelled out or it does not happen.
func parseCheckRunMode(value string, set bool) (CheckRunMode, error) {
	if !set {
		return ModeShadow, nil
	}
	switch value {
	case ModeShadow.String():
		return ModeShadow, nil
	case ModeAuthoritative.String():
		return ModeAuthoritative, nil
	default:
		return ModeShadow, &EnvConfigError{
			Variable: EnvCheckRunMode,
			Problem: fmt.Sprintf("must be %q or %q",
				ModeShadow.String(), ModeAuthoritative.String()),
		}
	}
}

// readCorrespondenceFile parses the declaration as a JSON object of task to
// Actions job.
//
// It is read key by key rather than decoded straight into a map because
// encoding/json resolves a repeated key by keeping the last one. A file
// naming a task twice would then be accepted, with nothing saying which job
// was used, and the correspondence an operator reviewed in the diff would not
// be the one running.
func readCorrespondenceFile(path string) (map[string]string, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("%w: %s: %v", ErrCheckRunConfigUnreadable, path, err)
	}
	defer file.Close()

	decoder := json.NewDecoder(io.LimitReader(file, maxCorrespondenceFileBytes+1))
	opening, err := decoder.Token()
	if err != nil {
		return nil, fmt.Errorf("%w: %s: %v", ErrCheckRunConfigUnreadable, path, err)
	}
	if delimiter, ok := opening.(json.Delim); !ok || delimiter != '{' {
		return nil, fmt.Errorf("%w: %s: must be a JSON object of task to Actions job",
			ErrCheckRunConfigUnreadable, path)
	}
	pairs := make(map[string]string)
	for decoder.More() {
		key, err := decoder.Token()
		if err != nil {
			return nil, fmt.Errorf("%w: %s: %v", ErrCheckRunConfigUnreadable, path, err)
		}
		task, ok := key.(string)
		if !ok {
			return nil, fmt.Errorf("%w: %s: task names must be strings",
				ErrCheckRunConfigUnreadable, path)
		}
		if _, repeated := pairs[task]; repeated {
			return nil, fmt.Errorf("%w: %s: task %q is declared twice",
				ErrCheckRunConfigUnreadable, path, task)
		}
		var job string
		if err := decoder.Decode(&job); err != nil {
			return nil, fmt.Errorf("%w: %s: Actions job for task %q must be a string",
				ErrCheckRunConfigUnreadable, path, task)
		}
		pairs[task] = job
	}
	if _, err := decoder.Token(); err != nil {
		return nil, fmt.Errorf("%w: %s: %v", ErrCheckRunConfigUnreadable, path, err)
	}
	// Anything after the object is a second document, which means the file
	// is not the one declaration it is read as.
	if decoder.More() {
		return nil, fmt.Errorf("%w: %s: trailing content after the correspondence",
			ErrCheckRunConfigUnreadable, path)
	}
	if decoder.InputOffset() > maxCorrespondenceFileBytes {
		return nil, fmt.Errorf("%w: %s: larger than %d bytes",
			ErrCheckRunConfigUnreadable, path, maxCorrespondenceFileBytes)
	}
	return pairs, nil
}

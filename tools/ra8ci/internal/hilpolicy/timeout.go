// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package hilpolicy selects bounded HIL observation deadlines from declared
// configuration and completed historical observations.
package hilpolicy

import (
	"bufio"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

const (
	DefaultSeconds = 30
	MaximumSeconds = 3600
	MinimumSamples = 5
)

var appName = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]*$`)

// Observation is one terminal, evidence-complete HIL observe step. Timed-out
// observations are right-censored and require additional headroom.
type Observation struct {
	Duration time.Duration
	TimedOut bool
}

// Decision records the chosen bound and its evidence for audit.
type Decision struct {
	Seconds                int     `json:"seconds"`
	Source                 string  `json:"source"`
	Samples                int     `json:"samples"`
	MeanSeconds            float64 `json:"mean_seconds,omitempty"`
	MaximumObservedSeconds float64 `json:"maximum_observed_seconds,omitempty"`
	StddevSeconds          float64 `json:"stddev_seconds,omitempty"`
	Censored               int     `json:"censored"`
	// EvidenceSeconds is what the observations alone demanded, before the
	// declared floor or the MaximumSeconds ceiling was applied, and the two
	// flags name whichever of those overruled them. Set on the observed path
	// only; see what_chose_the_bound.go.
	EvidenceSeconds  float64 `json:"evidence_seconds,omitempty"`
	RaisedToFallback bool    `json:"raised_to_fallback,omitempty"`
	ClippedToMaximum bool    `json:"clipped_to_maximum,omitempty"`
}

// DeclaredTimeout reads the existing app's HIL_TIMEOUT_S setting without
// evaluating shell syntax. Absent configuration returns found=false.
func DeclaredTimeout(root, app string) (seconds int, found bool, err error) {
	if !appName.MatchString(app) {
		return 0, false, errors.New("invalid HIL app name")
	}
	base := filepath.Join(root, "examples", "ek_ra8d2", "hw_validated", "hil")
	resolvedBase, err := filepath.EvalSymlinks(base)
	if err != nil {
		return 0, false, err
	}
	path := filepath.Join(base, app, "hil.conf")
	resolved, err := filepath.EvalSymlinks(path)
	if errors.Is(err, os.ErrNotExist) {
		absent, absentErr := configIsAbsent(filepath.Join(base, app), path)
		if absentErr != nil {
			return 0, false, absentErr
		}
		if !absent {
			return 0, false, fmt.Errorf("%w: %s", ErrUnresolvableConfig, path)
		}
		return 0, false, nil
	}
	if err != nil {
		return 0, false, err
	}
	if !strings.HasPrefix(resolved, resolvedBase+string(filepath.Separator)) {
		return 0, false, errors.New("HIL config escapes approved root")
	}
	file, err := os.Open(resolved)
	if err != nil {
		return 0, false, err
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, raw, hasEquals := strings.Cut(line, "=")
		if !hasEquals {
			if lineDeclaresTheTimeoutWithoutAValue(line) {
				return 0, false, fmt.Errorf("%w: %s states HIL_TIMEOUT_S as %q, assigning nothing", ErrUnreadableDeclaration, path, line)
			}
			continue
		}
		if trimmed := strings.TrimSpace(key); trimmed != "HIL_TIMEOUT_S" {
			if keyNamesTheTimeout(trimmed) {
				return 0, false, fmt.Errorf("%w: %s declares HIL_TIMEOUT_S as %q", ErrUnreadableDeclaration, path, trimmed)
			}
			continue
		}
		// The key spells the name and nothing else, so what is left to ask is
		// whether the line assigns at all. It is asked here rather than before
		// the spelling door so that an unread spelling is reported as the
		// spelling it is.
		if !shellAssignsHere(key, raw) {
			return 0, false, fmt.Errorf("%w: %s writes HIL_TIMEOUT_S as %q, which a shell sourcing it does not assign", ErrUnreadableDeclaration, path, line)
		}
		if found {
			return 0, false, errors.New("duplicate HIL_TIMEOUT_S")
		}
		value, convErr := strconv.Atoi(strings.TrimSpace(raw))
		if convErr != nil || value < 1 || value > MaximumSeconds {
			return 0, false, fmt.Errorf("invalid HIL_TIMEOUT_S in %s", path)
		}
		seconds, found = value, true
	}
	if err := scanner.Err(); err != nil {
		return 0, false, err
	}
	return seconds, found, nil
}

// Choose uses max + max(population standard deviation, max-mean), with
// right-censored failures demanding at least 1.5x their observed duration.
// Until five valid observations exist, it falls back to HIL_TIMEOUT_S or 30s.
func Choose(declaredSeconds int, declaredFound bool, observations []Observation) (Decision, error) {
	if declaredFound && (declaredSeconds < 1 || declaredSeconds > MaximumSeconds) {
		return Decision{}, errors.New("declared HIL timeout is out of bounds")
	}
	fallback := DefaultSeconds
	source := "default"
	if declaredFound {
		fallback, source = declaredSeconds, "hil.conf"
	}
	decision := Decision{Seconds: fallback, Source: source}
	var sum, maxDuration float64
	for _, sample := range observations {
		if sample.Duration <= 0 || sample.Duration > time.Duration(MaximumSeconds)*time.Second {
			return Decision{}, errors.New("invalid HIL observation")
		}
		seconds := sample.Duration.Seconds()
		if seconds > maxDuration {
			maxDuration = seconds
		}
		sum += seconds
		decision.Samples++
		if sample.TimedOut {
			decision.Censored++
		}
	}
	if decision.Samples < MinimumSamples {
		return decision, nil
	}
	mean := sum / float64(decision.Samples)
	var variance float64
	for _, sample := range observations {
		delta := sample.Duration.Seconds() - mean
		variance += delta * delta
	}
	stddev := math.Sqrt(variance / float64(decision.Samples))
	headroom := math.Max(stddev, maxDuration-mean)
	evidence := maxDuration + headroom
	if decision.Censored > 0 {
		evidence = math.Max(evidence, maxDuration*1.5)
	}
	decision.EvidenceSeconds = evidence
	decision.Seconds, decision.RaisedToFallback, decision.ClippedToMaximum = boundFromEvidence(evidence, fallback)
	decision.Source = "observed"
	decision.MeanSeconds = mean
	decision.MaximumObservedSeconds = maxDuration
	decision.StddevSeconds = stddev
	return decision, nil
}

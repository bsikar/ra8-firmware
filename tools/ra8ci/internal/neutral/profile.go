// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

const (
	ProfileVersion  = 1
	MaxProfileBytes = 64 << 10
)

var (
	ErrInvalidProfile = errors.New("invalid board neutral profile")
	profileNameRE     = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}$`)
	profilePathRE     = regexp.MustCompile(`^[a-zA-Z0-9_./+-]{1,256}$`)
)

// Profile is a reviewed, data-only description of observations required before
// the board may be handed to another holder. It contains no executable fields.
type Profile struct {
	SchemaVersion      int           `json:"schema_version"`
	BoardID            string        `json:"board_id"`
	FixtureRevision    string        `json:"fixture_revision"`
	RestorePolicy      string        `json:"restore_policy"`
	Identity           []SignalCheck `json:"identity"`
	State              []SignalCheck `json:"state"`
	Sensors            []RangeCheck  `json:"sensors"`
	ProtectedProcesses []string      `json:"protected_processes,omitempty"`
	ProtectedDevices   []string      `json:"protected_devices"`
}

// SignalCheck names a fixed observation source and its exact expected value.
// Sources are code-defined adapters such as sysfs or the authenticated Tapo
// reader; profile data can never name a command, URL, or arbitrary plugin.
type SignalCheck struct {
	Name     string `json:"name"`
	Source   string `json:"source"`
	Target   string `json:"target"`
	Expected string `json:"expected"`
}

// RangeCheck specifies an inclusive numeric interval for a named read-only
// sensor, in the units documented by that source (for example, millivolts).
type RangeCheck struct {
	Name   string  `json:"name"`
	Source string  `json:"source"`
	Target string  `json:"target"`
	Min    float64 `json:"min"`
	Max    float64 `json:"max"`
}

// LoadProfile reads a non-writable, regular profile and returns its exact
// content digest. The caller must install it from a reviewed deployment source.
func LoadProfile(path string) (Profile, string, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() <= 0 || info.Size() > MaxProfileBytes ||
		info.Mode().Perm()&0022 != 0 {
		return Profile{}, "", ErrInvalidProfile
	}
	file, err := os.Open(path)
	if err != nil {
		return Profile{}, "", err
	}
	defer file.Close()
	openedInfo, statErr := file.Stat()
	if statErr != nil || !os.SameFile(info, openedInfo) || !openedInfo.Mode().IsRegular() || openedInfo.Mode().Perm()&0022 != 0 {
		return Profile{}, "", ErrInvalidProfile
	}
	raw, err := io.ReadAll(io.LimitReader(file, MaxProfileBytes+1))
	if err != nil || len(raw) == 0 || len(raw) > MaxProfileBytes {
		return Profile{}, "", ErrInvalidProfile
	}
	var profile Profile
	decoder := json.NewDecoder(bytes.NewReader(raw))
	if err := inspectProfileJSON(decoder); err != nil {
		return Profile{}, "", fmt.Errorf("%w: invalid profile JSON: %v", ErrInvalidProfile, err)
	}
	if err := expectProfileJSONEOF(decoder); err != nil {
		return Profile{}, "", err
	}
	decoder = json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&profile); err != nil {
		return Profile{}, "", fmt.Errorf("%w: %v", ErrInvalidProfile, err)
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		return Profile{}, "", fmt.Errorf("%w: trailing data", ErrInvalidProfile)
	}
	if err := ValidateProfile(profile); err != nil {
		return Profile{}, "", err
	}
	digest := sha256.Sum256(raw)
	return profile, hex.EncodeToString(digest[:]), nil
}

func inspectProfileJSON(decoder *json.Decoder) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, ok := token.(json.Delim)
	if !ok {
		return nil
	}
	switch delimiter {
	case '{':
		seen := make(map[string]struct{})
		for decoder.More() {
			keyToken, err := decoder.Token()
			if err != nil {
				return err
			}
			key, ok := keyToken.(string)
			if !ok {
				return errors.New("object key is not a string")
			}
			if _, exists := seen[key]; exists {
				return fmt.Errorf("duplicate object key %q", key)
			}
			seen[key] = struct{}{}
			if err := inspectProfileJSON(decoder); err != nil {
				return err
			}
		}
	case '[':
		for decoder.More() {
			if err := inspectProfileJSON(decoder); err != nil {
				return err
			}
		}
	default:
		return fmt.Errorf("unexpected JSON delimiter %q", delimiter)
	}
	end, err := decoder.Token()
	if err != nil {
		return err
	}
	if end != json.Delim(delimiter+2) {
		return fmt.Errorf("mismatched JSON delimiter %q", end)
	}
	return nil
}

func expectProfileJSONEOF(decoder *json.Decoder) error {
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		return fmt.Errorf("%w: trailing data", ErrInvalidProfile)
	}
	return nil
}

// ValidateProfile rejects incomplete or ambiguous profiles before they can
// be used to sign an observation.
func ValidateProfile(profile Profile) error {
	if profile.SchemaVersion != ProfileVersion || !validBoardID(profile.BoardID) ||
		!profileNameRE.MatchString(profile.FixtureRevision) ||
		strings.TrimSpace(profile.RestorePolicy) != profile.RestorePolicy ||
		profile.RestorePolicy == "" || len(profile.RestorePolicy) > 256 ||
		len(profile.Identity) < 2 || len(profile.State) < 3 ||
		len(profile.Sensors) == 0 || len(profile.ProtectedDevices) == 0 {
		return ErrInvalidProfile
	}
	identity, err := validateSignalSet(profile.Identity, "identity")
	if err != nil || !identity["board-id"] || !identity["probe-id"] {
		return ErrInvalidProfile
	}
	state, err := validateSignalSet(profile.State, "state")
	if err != nil || !state["board-power"] || !state["reset"] || !state["relay"] {
		return ErrInvalidProfile
	}
	seenSensors := make(map[string]bool, len(profile.Sensors))
	for _, sensor := range profile.Sensors {
		if !profileNameRE.MatchString(sensor.Name) || seenSensors[sensor.Name] ||
			(sensor.Source != "sysfs" && sensor.Source != "sensor") ||
			(sensor.Source == "sensor" && sensor.Target != "board.vtref_millivolts") ||
			!validRelativePath(sensor.Target) ||
			sensor.Min != sensor.Min || sensor.Max != sensor.Max ||
			sensor.Min > sensor.Max || sensor.Min < -1e12 || sensor.Max > 1e12 {
			return ErrInvalidProfile
		}
		seenSensors[sensor.Name] = true
	}
	if !seenSensors["vtref-millivolts"] {
		return ErrInvalidProfile
	}
	seenDevices := make(map[string]bool, len(profile.ProtectedDevices))
	for _, device := range profile.ProtectedDevices {
		if !validRelativePath(device) || seenDevices[device] {
			return ErrInvalidProfile
		}
		seenDevices[device] = true
	}
	seenProcesses := make(map[string]bool, len(profile.ProtectedProcesses))
	for _, name := range profile.ProtectedProcesses {
		if !profileNameRE.MatchString(name) || seenProcesses[strings.ToLower(name)] {
			return ErrInvalidProfile
		}
		seenProcesses[strings.ToLower(name)] = true
	}
	return nil
}

func validateSignalSet(checks []SignalCheck, set string) (map[string]bool, error) {
	seen := make(map[string]bool, len(checks))
	for _, check := range checks {
		if !profileNameRE.MatchString(check.Name) || seen[check.Name] ||
			(check.Source != "sysfs" && check.Source != "tapo") ||
			!validRelativePath(check.Target) || strings.TrimSpace(check.Expected) != check.Expected ||
			check.Expected == "" || len(check.Expected) > 256 {
			return nil, fmt.Errorf("%w: invalid %s signal", ErrInvalidProfile, set)
		}
		if check.Source == "tapo" && check.Target != "board.power_state" {
			return nil, fmt.Errorf("%w: unsupported Tapo signal", ErrInvalidProfile)
		}
		seen[check.Name] = true
	}
	return seen, nil
}

func validRelativePath(value string) bool {
	if !profilePathRE.MatchString(value) || filepath.IsAbs(value) || filepath.Clean(value) != value ||
		value == "." || strings.HasPrefix(value, "..") {
		return false
	}
	for _, part := range strings.Split(value, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return true
}

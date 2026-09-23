// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"
	"unicode"
)

const (
	EnvConfigURL      = "RA8CI_GITHUB_CONFIG_URL"
	EnvAppClientID    = "RA8CI_GITHUB_APP_CLIENT_ID"
	EnvInstallationID = "RA8CI_GITHUB_INSTALLATION_ID"
	EnvPrivateKeyFile = "RA8CI_GITHUB_PRIVATE_KEY_FILE"
	EnvOwner          = "RA8CI_GITHUB_OWNER"
	EnvScaleSetID     = "RA8CI_GITHUB_SCALE_SET_ID"
	EnvMaxRunners     = "RA8CI_GITHUB_MAX_RUNNERS"
)

var decimalEnvironmentInteger = regexp.MustCompile(`^(0|[1-9][0-9]*)$`)

var sessionEnvironmentNames = []string{
	EnvConfigURL,
	EnvAppClientID,
	EnvInstallationID,
	EnvPrivateKeyFile,
	EnvOwner,
	EnvScaleSetID,
	EnvMaxRunners,
}

// EnvConfigError identifies the exact environment variable that is missing
// or malformed. It never includes private-key contents.
type EnvConfigError struct {
	Variable string
	Problem  string
}

func (e *EnvConfigError) Error() string {
	return fmt.Sprintf("GitHub scale-set environment %s: %s", e.Variable, e.Problem)
}

// LoadSessionConfigFromEnv returns enabled=false only when every supported
// variable is absent. It reads configuration values but never opens the
// private key; OpenSession owns protected-file and credential validation.
func LoadSessionConfigFromEnv() (config SessionConfig, enabled bool, err error) {
	values := make(map[string]string, len(sessionEnvironmentNames))
	present := make(map[string]bool, len(sessionEnvironmentNames))
	anyPresent := false
	for _, name := range sessionEnvironmentNames {
		value, ok := os.LookupEnv(name)
		values[name], present[name] = value, ok
		anyPresent = anyPresent || ok
	}
	if !anyPresent {
		return SessionConfig{}, false, nil
	}
	for _, name := range sessionEnvironmentNames {
		if !present[name] {
			return SessionConfig{}, false, &EnvConfigError{Variable: name, Problem: "required when GitHub scale-set integration is configured"}
		}
		if values[name] == "" {
			return SessionConfig{}, false, &EnvConfigError{Variable: name, Problem: "must not be empty"}
		}
		if strings.IndexFunc(values[name], unicode.IsSpace) >= 0 {
			return SessionConfig{}, false, &EnvConfigError{Variable: name, Problem: "must not contain whitespace"}
		}
	}

	installationID, err := parseEnvironmentInt(EnvInstallationID, values[EnvInstallationID], 1, int64(^uint64(0)>>1))
	if err != nil {
		return SessionConfig{}, false, err
	}
	maxInt := int64(^uint(0) >> 1)
	scaleSetID, err := parseEnvironmentInt(EnvScaleSetID, values[EnvScaleSetID], 1, maxInt)
	if err != nil {
		return SessionConfig{}, false, err
	}
	maxRunners, err := parseEnvironmentInt(EnvMaxRunners, values[EnvMaxRunners], 0, 10000)
	if err != nil {
		return SessionConfig{}, false, err
	}
	return SessionConfig{
		GitHubConfigURL: values[EnvConfigURL],
		AppClientID:     values[EnvAppClientID],
		InstallationID:  installationID,
		PrivateKeyFile:  values[EnvPrivateKeyFile],
		Owner:           values[EnvOwner],
		ScaleSetID:      int(scaleSetID),
		MaxRunners:      int(maxRunners),
	}, true, nil
}

func parseEnvironmentInt(name, value string, minimum, maximum int64) (int64, error) {
	if !decimalEnvironmentInteger.MatchString(value) {
		return 0, &EnvConfigError{Variable: name, Problem: "must be a canonical base-10 integer"}
	}
	parsed, err := strconv.ParseInt(value, 10, 64)
	if err != nil {
		return 0, &EnvConfigError{Variable: name, Problem: "is outside the supported integer range"}
	}
	if parsed < minimum || parsed > maximum {
		return 0, &EnvConfigError{Variable: name, Problem: fmt.Sprintf("must be between %d and %d", minimum, maximum)}
	}
	return parsed, nil
}

// IsEnvConfigError supports callers that want to distinguish disabled
// integration from an operator configuration error without string matching.
func IsEnvConfigError(err error) bool {
	var target *EnvConfigError
	return errors.As(err, &target)
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"fmt"
	"os"
	"strings"
	"unicode"
)

// check_run_env_config.go reads which mode a publisher runs in and which
// Actions job covers which catalog task. Neither of those builds a publisher:
// NewCheckRunPublisher also needs App credentials and the repository whose
// check runs it posts, and nothing read them, so a deployment could declare a
// correspondence it had no way to publish against.
//
// The App credentials are the scale-set ones, read under the names
// env_config.go already defines, because there is one App and one
// installation. A second spelling of RA8CI_GITHUB_APP_CLIENT_ID would be a
// second place for one deployment to disagree with itself. The repository is
// the publisher's own variable, because a scale set is not a repository and
// the session configuration never names one.

// EnvCheckRunRepository names the repository whose check runs this process
// publishes. Setting it is what turns publishing on.
const EnvCheckRunRepository = "RA8CI_GITHUB_CHECK_RUN_REPOSITORY"

// checkRunCredentialNames are the App credentials a check-run publisher
// shares with the scale-set session.
var checkRunCredentialNames = []string{
	EnvAppClientID,
	EnvInstallationID,
	EnvPrivateKeyFile,
	EnvOwner,
}

// LoadCheckRunPublisherConfigFromEnv reads the credentials a check-run
// publisher needs, returning enabled=false when EnvCheckRunRepository is
// absent.
//
// Publishing keys on that one variable rather than on any credential being
// present, because the credentials belong to the scale-set session too: a
// deployment that runs the session and publishes nothing must report disabled
// rather than a half-configured publisher missing a repository.
//
// mode is passed in rather than read here. LoadCheckRunConfigFromEnv already
// resolves EnvCheckRunMode, and a second read is a second place for the
// answer to differ.
//
// Nothing here opens the private key. NewCheckRunPublisher owns protected-file
// and key validation, the division LoadSessionConfigFromEnv already draws.
func LoadCheckRunPublisherConfigFromEnv(mode CheckRunMode) (CheckRunPublisherConfig, bool, error) {
	repository, set := os.LookupEnv(EnvCheckRunRepository)
	if !set {
		return CheckRunPublisherConfig{}, false, nil
	}
	// The mode is checked before the environment is read further, so a
	// process asking for a mode this build will not run refuses without
	// having looked at credentials.
	if mode != ModeShadow && mode != ModeAuthoritative {
		return CheckRunPublisherConfig{}, false, fmt.Errorf("%w: %s", ErrInvalidCheckRunMode, mode)
	}
	if err := usableEnvironmentValue(EnvCheckRunRepository, repository, true); err != nil {
		return CheckRunPublisherConfig{}, false, err
	}
	if !repositoryPart.MatchString(repository) {
		return CheckRunPublisherConfig{}, false, &EnvConfigError{
			Variable: EnvCheckRunRepository,
			Problem:  "must be a repository name on its own, not an owner/name pair",
		}
	}

	values := make(map[string]string, len(checkRunCredentialNames))
	for _, name := range checkRunCredentialNames {
		value, present := os.LookupEnv(name)
		if err := usableEnvironmentValue(name, value, present); err != nil {
			return CheckRunPublisherConfig{}, false, err
		}
		values[name] = value
	}
	if !ownerName.MatchString(values[EnvOwner]) {
		return CheckRunPublisherConfig{}, false, &EnvConfigError{
			Variable: EnvOwner,
			Problem:  "must be a GitHub account name",
		}
	}
	installationID, err := parseEnvironmentInt(EnvInstallationID, values[EnvInstallationID], 1, int64(^uint64(0)>>1))
	if err != nil {
		return CheckRunPublisherConfig{}, false, err
	}
	return CheckRunPublisherConfig{
		AppClientID:    values[EnvAppClientID],
		InstallationID: installationID,
		PrivateKeyFile: values[EnvPrivateKeyFile],
		Owner:          values[EnvOwner],
		Repository:     repository,
		Mode:           mode,
	}, true, nil
}

// usableEnvironmentValue applies the rule LoadSessionConfigFromEnv applies to
// each of its variables: present, not empty, no whitespace. The refusal names
// the variable and never the value, since one of these is a path and another
// is an account name.
func usableEnvironmentValue(name, value string, present bool) error {
	switch {
	case !present:
		return &EnvConfigError{Variable: name, Problem: "required when check-run publishing is configured"}
	case value == "":
		return &EnvConfigError{Variable: name, Problem: "must not be empty"}
	case strings.IndexFunc(value, unicode.IsSpace) >= 0:
		return &EnvConfigError{Variable: name, Problem: "must not contain whitespace"}
	}
	return nil
}

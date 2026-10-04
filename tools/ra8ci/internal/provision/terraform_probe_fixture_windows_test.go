//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

type terraformProbeFixture struct {
	Body   string `json:"body"`
	Record string `json:"record,omitempty"`
}

func installTerraformProbe(t *testing.T, directory, body string) (string, error) {
	path := filepath.Join(directory, "terraform.exe")
	if err := copyTestExecutable(path); err != nil {
		return "", err
	}
	fixture := terraformProbeFixture{Body: body}
	if start := strings.Index(body, "pwd > '"); start >= 0 {
		value := body[start+len("pwd > '"):]
		if end := strings.IndexByte(value, '\''); end >= 0 {
			fixture.Record = value[:end]
		}
	}
	encoded, err := json.Marshal(fixture)
	if err != nil {
		return "", err
	}
	if err := os.WriteFile(terraformProbeFixturePath(path), encoded, 0o600); err != nil {
		return "", err
	}
	return path, nil
}

func copyTestExecutable(path string) error {
	source, err := os.Executable()
	if err != nil {
		return err
	}
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	output, err := os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o700)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(output, input)
	closeErr := output.Close()
	if err := errors.Join(copyErr, closeErr); err != nil {
		return err
	}
	return testprivatefile.OwnerOnly(path)
}

func dispatchTerraformProbe() {
	path := os.Args[0]
	if !strings.EqualFold(filepath.Base(path), "terraform.exe") {
		return
	}
	encoded, err := os.ReadFile(terraformProbeFixturePath(path))
	if err != nil {
		os.Exit(81)
	}
	var fixture terraformProbeFixture
	if json.Unmarshal(encoded, &fixture) != nil {
		os.Exit(82)
	}
	body := fixture.Body
	if len(os.Args) > 1 && os.Args[1] == "version" {
		probeBody := body
		if start := strings.Index(body, `if [ "$1" = "version" ]`); start >= 0 {
			if end := strings.Index(body[start:], "; fi\n"); end >= 0 {
				probeBody = body[start : start+end+4]
			}
		}
		stdout, stderr, exitCode := terraformProbeResponse(probeBody)
		if fixture.Record != "" {
			cwd, _ := os.Getwd()
			_ = os.WriteFile(fixture.Record, []byte(cwd+"\n"), 0o600)
		}
		_, _ = io.WriteString(os.Stdout, stdout)
		_, _ = io.WriteString(os.Stderr, stderr)
		os.Exit(exitCode)
	}

	terraformRecord := terraformRecordDirectory(body)
	if terraformRecord != "" {
		cwd, _ := os.Getwd()
		_ = os.WriteFile(filepath.Join(terraformRecord, "cwd"), []byte(cwd+"\n"), 0o600)
		_ = os.WriteFile(filepath.Join(terraformRecord, "args"), []byte(strings.Join(os.Args[1:], "\n")+"\n"), 0o600)
		_ = os.WriteFile(filepath.Join(terraformRecord, "env"), []byte(strings.Join(os.Environ(), "\n")+"\n"), 0o600)
	}
	for _, arg := range os.Args[1:] {
		if strings.HasPrefix(arg, "-out=") {
			_ = os.WriteFile(strings.TrimPrefix(arg, "-out="), []byte("PLAN"), 0o600)
		}
	}
	tail := terraformCommandTail(body)
	if strings.Contains(tail, "sleep 30") {
		time.Sleep(30 * time.Second)
	}
	stdout, _, exitCode := terraformProbeResponse(tail)
	_, _ = io.WriteString(os.Stdout, stdout)
	os.Exit(exitCode)
}

func terraformProbeFixturePath(path string) string { return path + ".fixture.json" }

func terraformProbeResponse(body string) (string, string, int) {
	exitCode := 0
	if strings.Contains(body, "exit 1") || strings.Contains(body, "exit 3") {
		exitCode = 1
	}
	if strings.Contains(body, "yes a | head -c 70000") {
		return strings.Repeat("a", 70000), "", exitCode
	}
	start := strings.Index(body, "printf '%s' '")
	if start < 0 {
		return "", "", exitCode
	}
	value := body[start+len("printf '%s' '"):]
	end := strings.IndexByte(value, '\'')
	if end < 0 {
		return "", "", exitCode
	}
	output := value[:end]
	if strings.Contains(body[start:], "1>&2") {
		return "", output, exitCode
	}
	return output, "", exitCode
}

func terraformRecordDirectory(body string) string {
	marker := "pwd > "
	start := strings.Index(body, marker)
	if start < 0 {
		return ""
	}
	value := strings.TrimSpace(body[start+len(marker):])
	value, _, _ = strings.Cut(value, "\n")
	value = strings.Trim(value, "'\"\n")
	return filepath.Dir(value)
}

func terraformCommandTail(body string) string {
	marker := "env > "
	start := strings.Index(body, marker)
	if start < 0 {
		return body
	}
	end := strings.Index(body[start:], "\n")
	if end < 0 {
		return ""
	}
	return body[start+end+1:]
}

func init() { dispatchTerraformProbe() }

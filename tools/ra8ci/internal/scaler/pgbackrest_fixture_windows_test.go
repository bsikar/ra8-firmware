//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

type pgBackRestFixture struct {
	Body string `json:"body"`
}

func installPgBackRestFixture(t *testing.T, directory, body string, _ os.FileMode) (string, error) {
	path := filepath.Join(directory, "pgbackrest.exe")
	if err := copyTestExecutable(path); err != nil {
		return "", err
	}
	return path, writePgBackRestFixture(path, body)
}

func rewritePgBackRestFixture(_ *testing.T, path, body string, _ os.FileMode) error {
	return writePgBackRestFixture(path, body)
}

func writePgBackRestFixture(path, body string) error {
	data, err := json.Marshal(pgBackRestFixture{Body: body})
	if err != nil {
		return err
	}
	return os.WriteFile(pgBackRestFixturePath(path), data, 0o600)
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
	output, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o700)
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

func dispatchPgBackRestFixture() {
	path := os.Args[0]
	if !strings.EqualFold(filepath.Base(path), "pgbackrest.exe") {
		return
	}
	data, err := os.ReadFile(pgBackRestFixturePath(path))
	if err != nil {
		os.Exit(81)
	}
	var fixture pgBackRestFixture
	if json.Unmarshal(data, &fixture) != nil {
		os.Exit(82)
	}
	if strings.Contains(fixture.Body, "exit 3") {
		os.Exit(3)
	}
	if strings.Contains(fixture.Body, `"name":"other"`) {
		_, _ = io.WriteString(os.Stdout, `[{"name":"other","backup":[{"type":"full","timestamp":{"stop":1}}]}]`)
		os.Exit(0)
	}
	_, _ = io.WriteString(os.Stdout, `[{"name":"ra8ci","backup":[{"type":"full","timestamp":{"stop":`+strconv.FormatInt(time.Now().Unix(), 10)+`}}]}]`)
	os.Exit(0)
}

func pgBackRestFixturePath(path string) string { return path + ".fixture.json" }

func init() { dispatchPgBackRestFixture() }

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

const maxHILCaptureBytes = 8 << 20

type hilCaptureVerification struct {
	Manifest     string `json:"manifest"`
	Mode         string `json:"mode"`
	CaptureBytes int    `json:"capture_bytes"`
	Accepted     bool   `json:"accepted"`
}

func hilVerifyCaptureCommand(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("hil verify-capture", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	manifest := flags.String("manifest", "", "HIL manifest beneath examples/")
	capturePath := flags.String("capture", "", "captured UART/RTT output file")
	if err := flags.Parse(args); err != nil {
		return fmt.Errorf("usage: ra8ci hil verify-capture --manifest examples/.../hil.conf --capture FILE: %w", err)
	}
	if flags.NArg() != 0 || *manifest == "" || *capturePath == "" {
		return errors.New("usage: ra8ci hil verify-capture --manifest examples/.../hil.conf --capture FILE")
	}
	if ctx == nil || ctx.Err() != nil {
		return errors.New("HIL capture verification requires an active context")
	}
	info, err := os.Lstat(*capturePath)
	if err != nil || !info.Mode().IsRegular() || info.Size() > maxHILCaptureBytes {
		return errors.New("capture must be a regular file no larger than 8 MiB")
	}
	capture, err := os.Open(*capturePath)
	if err != nil {
		return fmt.Errorf("open HIL capture: %w", err)
	}
	defer capture.Close()
	opened, err := capture.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) || opened.Size() > maxHILCaptureBytes {
		return errors.New("HIL capture changed or became unsafe while opening")
	}
	root, err := findCheckout()
	if err != nil {
		return err
	}
	result, err := verifyHILCapture(root, *manifest, capture)
	if err != nil {
		return err
	}
	return json.NewEncoder(os.Stdout).Encode(result)
}

func verifyHILCapture(root, manifest string, captured io.Reader) (hilCaptureVerification, error) {
	if strings.TrimSpace(root) != root || root == "" || captured == nil {
		return hilCaptureVerification{}, errors.New("invalid HIL capture verification input")
	}
	spec, err := hilspec.Load(root, manifest)
	if err != nil {
		return hilCaptureVerification{}, fmt.Errorf("load HIL manifest: %w", err)
	}
	data, err := io.ReadAll(io.LimitReader(captured, maxHILCaptureBytes+1))
	if err != nil || len(data) > maxHILCaptureBytes {
		return hilCaptureVerification{}, errors.New("HIL capture is unreadable or exceeds 8 MiB")
	}
	if err := hilspec.VerifyTextCapture(spec, data); err != nil {
		return hilCaptureVerification{}, err
	}
	return hilCaptureVerification{Manifest: spec.Path, Mode: string(spec.Mode), CaptureBytes: len(data), Accepted: true}, nil
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"net/http"
	"os"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/syncclient"
)

// syncLocalRuns reports historical evidence. It cannot schedule a task, and
// it never upgrades an unverified local run into a trusted CI result.
func syncLocalRuns(ctx context.Context) error {
	endpoint, err := resolveClientEndpoint("ra8ci sync", roleOperator, os.Getenv)
	if err != nil {
		return err
	}
	caPEM, err := os.ReadFile(endpoint.CAFile)
	if err != nil {
		return fmt.Errorf("read server CA: %w", err)
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(caPEM) {
		return errors.New("server CA has no trusted certificate")
	}
	identity, err := mtls.LoadClientIdentity(endpoint.CertFile, endpoint.KeyFile, time.Now())
	if err != nil {
		return fmt.Errorf("sync client identity: %w", err)
	}
	transport := &http.Transport{Proxy: nil, TLSClientConfig: &tls.Config{
		MinVersion: tls.VersionTLS13, RootCAs: roots,
		Certificates: []tls.Certificate{identity},
	}}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: 30 * time.Second}
	directory, err := spool.DefaultDirectory()
	if err != nil {
		return err
	}
	outbox, err := spool.Open(directory)
	if err != nil {
		return err
	}
	report, err := syncclient.SyncPending(ctx, outbox, endpoint.ServerURL, client)
	if err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "ra8ci: synced %d local runs; %d legacy records need manual review\n", report.Synced, report.Quarantined)
	return nil
}

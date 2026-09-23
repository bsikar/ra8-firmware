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

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/syncclient"
)

// syncLocalRuns reports historical evidence. It cannot schedule a task, and
// it never upgrades an unverified local run into a trusted CI result.
func syncLocalRuns(ctx context.Context) error {
	serverURL := os.Getenv("RA8CI_SERVER_URL")
	caFile := os.Getenv("RA8CI_SERVER_CA")
	certFile := os.Getenv("RA8CI_CLIENT_CERT")
	keyFile := os.Getenv("RA8CI_CLIENT_KEY")
	if serverURL == "" || caFile == "" || certFile == "" || keyFile == "" {
		return errors.New("sync requires RA8CI_SERVER_URL, RA8CI_SERVER_CA, RA8CI_CLIENT_CERT, and RA8CI_CLIENT_KEY")
	}
	caPEM, err := os.ReadFile(caFile)
	if err != nil {
		return fmt.Errorf("read server CA: %w", err)
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(caPEM) {
		return errors.New("server CA has no trusted certificate")
	}
	identity, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		return fmt.Errorf("load sync client identity: %w", err)
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
	report, err := syncclient.SyncPending(ctx, outbox, serverURL, client)
	if err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "ra8ci: synced %d local runs; %d legacy records need manual review\n", report.Synced, report.Quarantined)
	return nil
}

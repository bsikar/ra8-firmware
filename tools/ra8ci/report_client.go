// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type slowReportPayload struct {
	Repository    string           `json:"repository"`
	WindowSeconds int64            `json:"window_seconds"`
	Tasks         []store.SlowTask `json:"tasks"`
}

// fetchSlowReport asks the server; a CLI on a laptop never opens PostgreSQL.
func fetchSlowReport(ctx context.Context, repository string, window time.Duration, limit int) (slowReportPayload, error) {
	endpoint, err := resolveClientEndpoint("ra8ci report slow", roleOperator, os.Getenv)
	if err != nil {
		return slowReportPayload{}, err
	}
	base, err := url.Parse(endpoint.ServerURL)
	if err != nil || base.Scheme != "https" || base.Host == "" || base.User != nil ||
		(base.Path != "" && base.Path != "/") || base.RawQuery != "" || base.Fragment != "" {
		return slowReportPayload{}, errors.New("RA8CI_SERVER_URL must be an HTTPS origin")
	}
	caPEM, err := os.ReadFile(endpoint.CAFile)
	if err != nil {
		return slowReportPayload{}, fmt.Errorf("read server CA: %w", err)
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(caPEM) {
		return slowReportPayload{}, errors.New("server CA has no trusted certificate")
	}
	identity, err := mtls.LoadClientIdentity(endpoint.CertFile, endpoint.KeyFile, time.Now())
	if err != nil {
		return slowReportPayload{}, fmt.Errorf("report client identity: %w", err)
	}
	base.Path = "/v1/reports/slow"
	query := url.Values{}
	query.Set("repository", repository)
	query.Set("window_seconds", fmt.Sprint(int64(window.Seconds())))
	query.Set("limit", fmt.Sprint(limit))
	base.RawQuery = query.Encode()
	transport := &http.Transport{Proxy: nil, TLSClientConfig: &tls.Config{
		MinVersion: tls.VersionTLS13, RootCAs: roots,
		Certificates: []tls.Certificate{identity},
	}}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: 30 * time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, base.String(), nil)
	if err != nil {
		return slowReportPayload{}, err
	}
	response, err := client.Do(request)
	if err != nil {
		return slowReportPayload{}, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return slowReportPayload{}, fmt.Errorf("slow report returned HTTP %d", response.StatusCode)
	}
	data, err := io.ReadAll(io.LimitReader(response.Body, (1<<20)+1))
	if err != nil || len(data) > 1<<20 {
		return slowReportPayload{}, errors.New("slow report exceeds response limit")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	var result slowReportPayload
	if err := decoder.Decode(&result); err != nil {
		return slowReportPayload{}, fmt.Errorf("decode slow report: %w", err)
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) ||
		result.Repository != repository || result.WindowSeconds != int64(window.Seconds()) || len(result.Tasks) > limit {
		return slowReportPayload{}, errors.New("slow report response does not match request")
	}
	return result, nil
}

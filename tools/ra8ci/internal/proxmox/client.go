// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package proxmox provides a narrow, identity-checked Proxmox VM lifecycle client.
// It never exposes a generic API request or a host command execution method.
package proxmox

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
)

const maxResponseBytes = 1 << 20

var (
	ErrInvalid        = errors.New("invalid Proxmox client input")
	ErrDenied         = errors.New("Proxmox access denied")
	ErrNotFound       = errors.New("Proxmox VM not found")
	ErrConflict       = errors.New("Proxmox VM identity conflict")
	ErrUnavailable    = errors.New("Proxmox API unavailable")
	ErrUnknownOutcome = errors.New("Proxmox mutation outcome unknown; reconcile before retry")
	ErrProtocol       = errors.New("invalid Proxmox API response")
)

var (
	idPattern     = regexp.MustCompile(`^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$`)
	namePattern   = regexp.MustCompile(`^ra8-lab-[a-z0-9][a-z0-9-]{0,54}$`)
	partPattern   = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$`)
	tokenPattern  = regexp.MustCompile(`^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+![A-Za-z0-9_.-]+=[A-Za-z0-9_-]+$`)
	bridgePattern = regexp.MustCompile(`^vmbr[0-9]{1,4}$`)
	upidPattern   = regexp.MustCompile(`^UPID:[A-Za-z0-9_.-]+:[A-Za-z0-9:@!_.-]+$`)
)

// Config is operator-controlled, never derived from a job payload or checkout.
// AllowedVMIDs and TemplateVMIDs are explicit reservations, not a broad range.
type Config struct {
	Endpoint         string
	CAFile           string
	TokenFile        string
	TokenEnv         string
	Node             string
	Pool             string
	Storage          string
	AllowedVMIDs     []int
	TemplateVMIDs    []int
	Bridges          []string
	RequestTimeout   time.Duration
	OperationTimeout time.Duration
	TaskPollInterval time.Duration
}

// Client does not expose its credentials, transport, or arbitrary API paths.
type Client struct {
	endpoint         url.URL
	httpClient       *http.Client
	token            string
	node             string
	pool             string
	storage          string
	allowedVMIDs     map[int]struct{}
	templateVMIDs    map[int]struct{}
	bridges          map[string]struct{}
	requestTimeout   time.Duration
	operationTimeout time.Duration
	pollInterval     time.Duration
}

// New requires a TLS CA and exactly one protected token source.
func New(cfg Config) (*Client, error) {
	u, err := url.Parse(cfg.Endpoint)
	if err != nil || u.Scheme != "https" || u.Hostname() == "" || u.Port() == "" || u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
		return nil, fmt.Errorf("%w: explicit HTTPS API origin with port required", ErrInvalid)
	}
	if isPersonalNetworkHost(u.Hostname()) {
		return nil, fmt.Errorf("%w: personal-network endpoint is prohibited", ErrInvalid)
	}
	if !partPattern.MatchString(cfg.Node) || !partPattern.MatchString(cfg.Pool) || !partPattern.MatchString(cfg.Storage) || cfg.Pool == "ra8ci-control" {
		return nil, fmt.Errorf("%w: node, disposable pool, and storage must be explicit", ErrInvalid)
	}
	allowed, err := checkedIDs(cfg.AllowedVMIDs)
	if err != nil || len(allowed) == 0 {
		return nil, fmt.Errorf("%w: explicit allowed VM IDs >= 9000 required", ErrInvalid)
	}
	templates, err := checkedIDs(cfg.TemplateVMIDs)
	if err != nil || len(templates) == 0 {
		return nil, fmt.Errorf("%w: explicit reviewed template IDs >= 9000 required", ErrInvalid)
	}
	if err := checkDisjointIDs(allowed, templates); err != nil {
		return nil, err
	}
	bridges, err := checkedBridges(cfg.Bridges)
	if err != nil {
		return nil, err
	}
	if (cfg.TokenFile == "") == (cfg.TokenEnv == "") {
		return nil, fmt.Errorf("%w: exactly one API token source is required", ErrInvalid)
	}
	token, err := loadToken(cfg.TokenFile, cfg.TokenEnv)
	if err != nil {
		return nil, err
	}
	caPEM, err := os.ReadFile(cfg.CAFile)
	if err != nil {
		return nil, fmt.Errorf("%w: load configured CA: %v", ErrInvalid, err)
	}
	roots, err := mtls.ServerAuthorities(caPEM, time.Now())
	if err != nil {
		return nil, fmt.Errorf("%w: configured Proxmox API CA: %v", ErrInvalid, err)
	}
	requestTimeout := cfg.RequestTimeout
	if requestTimeout == 0 {
		requestTimeout = 15 * time.Second
	}
	operationTimeout := cfg.OperationTimeout
	if operationTimeout == 0 {
		operationTimeout = 5 * time.Minute
	}
	pollInterval := cfg.TaskPollInterval
	if pollInterval == 0 {
		pollInterval = 500 * time.Millisecond
	}
	if requestTimeout < time.Millisecond || requestTimeout > 30*time.Second || operationTimeout < time.Millisecond || operationTimeout > 30*time.Minute || pollInterval < time.Millisecond || pollInterval > 30*time.Second {
		return nil, fmt.Errorf("%w: timeout outside bounded policy", ErrInvalid)
	}
	transport := &http.Transport{
		Proxy:                 nil,
		TLSClientConfig:       &tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS13},
		DialContext:           (&net.Dialer{Timeout: requestTimeout, KeepAlive: 30 * time.Second}).DialContext,
		TLSHandshakeTimeout:   requestTimeout,
		ResponseHeaderTimeout: requestTimeout,
		MaxIdleConnsPerHost:   2,
	}
	return &Client{
		endpoint: *u, httpClient: &http.Client{Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }},
		token: token, node: cfg.Node, pool: cfg.Pool, storage: cfg.Storage,
		allowedVMIDs: allowed, templateVMIDs: templates, bridges: bridges,
		requestTimeout: requestTimeout, operationTimeout: operationTimeout, pollInterval: pollInterval,
	}, nil
}

func checkedIDs(ids []int) (map[int]struct{}, error) {
	result := make(map[int]struct{}, len(ids))
	for _, id := range ids {
		if id < 9000 || id > 999999999 {
			return nil, ErrInvalid
		}
		if _, exists := result[id]; exists {
			return nil, ErrInvalid
		}
		result[id] = struct{}{}
	}
	return result, nil
}

// checkedBridges accepts only explicitly reviewed guest bridges. vmbr0 is
// refused by name: it is the Proxmox management bridge by convention, and a
// disposable guest that reaches the control plane's own network defeats every
// other boundary in this package.
func checkedBridges(names []string) (map[string]struct{}, error) {
	if len(names) == 0 {
		return nil, fmt.Errorf("%w: explicit reviewed guest bridges required", ErrInvalid)
	}
	checked := make(map[string]struct{}, len(names))
	for _, name := range names {
		if !bridgePattern.MatchString(name) || name == "vmbr0" {
			return nil, fmt.Errorf("%w: guest bridge %q is outside the reviewed disposable network", ErrInvalid, name)
		}
		if _, duplicate := checked[name]; duplicate {
			return nil, fmt.Errorf("%w: duplicate guest bridge %q", ErrInvalid, name)
		}
		checked[name] = struct{}{}
	}
	return checked, nil
}

func isPersonalNetworkHost(host string) bool {
	if strings.HasSuffix(strings.ToLower(host), ".ts.net") {
		return true
	}
	addr, err := netip.ParseAddr(host)
	return err == nil && netip.MustParsePrefix("100.64.0.0/10").Contains(addr)
}

func loadToken(path, envName string) (string, error) {
	var raw string
	if path != "" {
		info, err := os.Lstat(path)
		if err != nil || !info.Mode().IsRegular() || info.Size() > 4096 || info.Mode().Perm()&0077 != 0 {
			return "", fmt.Errorf("%w: token file must be private, regular, and <= 4096 bytes", ErrInvalid)
		}
		bytes, err := os.ReadFile(path)
		if err != nil {
			return "", fmt.Errorf("%w: token file unreadable", ErrInvalid)
		}
		raw = string(bytes)
	} else {
		if envName != "RA8CI_PROXMOX_API_TOKEN" {
			return "", fmt.Errorf("%w: unapproved token environment name", ErrInvalid)
		}
		raw = os.Getenv(envName)
	}
	token := strings.TrimSuffix(raw, "\n")
	if !tokenPattern.MatchString(token) {
		return "", fmt.Errorf("%w: malformed Proxmox API token", ErrInvalid)
	}
	return token, nil
}

type envelope struct {
	Data   json.RawMessage   `json:"data"`
	Errors map[string]string `json:"errors"`
}

func (c *Client) request(ctx context.Context, method, path string, form url.Values, out any) (int, error) {
	callCtx, cancel := context.WithTimeout(ctx, c.requestTimeout)
	defer cancel()
	u := c.endpoint
	pathPart, rawQuery, _ := strings.Cut(path, "?")
	u.Path = "/api2/json" + pathPart
	u.RawQuery = rawQuery
	var body io.Reader
	if form != nil {
		body = strings.NewReader(form.Encode())
	}
	req, err := http.NewRequestWithContext(callCtx, method, u.String(), body)
	if err != nil {
		return 0, fmt.Errorf("%w: construct request", ErrInvalid)
	}
	req.Header.Set("Authorization", "PVEAPIToken="+c.token)
	req.Header.Set("Accept", "application/json")
	if form != nil {
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return 0, fmt.Errorf("%w: transport or deadline: %v", ErrUnavailable, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		switch resp.StatusCode {
		case http.StatusUnauthorized, http.StatusForbidden:
			return resp.StatusCode, ErrDenied
		case http.StatusNotFound:
			return resp.StatusCode, ErrNotFound
		default:
			return resp.StatusCode, fmt.Errorf("%w: HTTP %d", ErrUnavailable, resp.StatusCode)
		}
	}
	if !strings.HasPrefix(resp.Header.Get("Content-Type"), "application/json") {
		return resp.StatusCode, ErrProtocol
	}
	raw, err := io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes+1))
	if err != nil || len(raw) > maxResponseBytes {
		return resp.StatusCode, ErrProtocol
	}
	var result envelope
	decoder := json.NewDecoder(bytes.NewReader(raw))
	if err := decoder.Decode(&result); err != nil || len(result.Data) == 0 || string(result.Data) == "null" || len(result.Errors) != 0 {
		return resp.StatusCode, ErrProtocol
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		return resp.StatusCode, ErrProtocol
	}
	if err := json.Unmarshal(result.Data, out); err != nil {
		return resp.StatusCode, ErrProtocol
	}
	return resp.StatusCode, nil
}

func vmPath(node string, id int) string {
	return "/nodes/" + node + "/qemu/" + strconv.Itoa(id)
}

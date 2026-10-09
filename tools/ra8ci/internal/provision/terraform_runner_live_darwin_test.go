//go:build darwin

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	goruntime "runtime"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/privatefile"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// liveRunnerConfig contains only endpoints and protected-file paths. The
// wrapper reads OpenBao and state-encryption secrets from the controller's
// protected store; this file never contains credential values.
type liveRunnerConfig struct {
	Terraform TerraformConfig
	Proxmox   struct {
		Endpoint  string
		CAFile    string
		TokenFile string
	}
	Runner struct {
		Actor             string
		OpenBaoAddress    string
		OpenBaoKVMount    string
		OpenBaoSecretPath string
		ModuleDirectory   string
	}
}

type liveRunnerLedger struct {
	vm      store.RunnerVM
	ops     map[string]store.RunnerVMOperation
	state   []byte
	opIndex int
}

func (l *liveRunnerLedger) GetRunnerVM(_ context.Context, id string) (store.RunnerVM, error) {
	if l.vm.ID == "" || l.vm.ID != id {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return l.vm, nil
}

func (l *liveRunnerLedger) GetRunnerVMByJob(_ context.Context, scaleSetID int64, jobID string) (store.RunnerVM, error) {
	if l.vm.ID == "" || l.vm.ScaleSetID != scaleSetID || l.vm.JobID != jobID {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return l.vm, nil
}

func (l *liveRunnerLedger) ReserveRunnerVM(_ context.Context, _ string, input store.RunnerVMInput,
	deadline time.Time) (store.RunnerVM, bool, error) {
	if l.vm.ID != "" || !deadline.After(time.Now()) {
		return store.RunnerVM{}, false, store.ErrConflict
	}
	l.vm = store.RunnerVM{ID: "0192f3a4-b5c6-7d8e-9f01-1234567890ab",
		RunnerVMInput: input, CreationOperationID: "0192f3a4-b5c6-7d8e-9f01-1234567890ac",
		State: "reserved", Generation: 1}
	return l.vm, true, nil
}

func (l *liveRunnerLedger) ListActiveRunnerVMIDs(context.Context, string) ([]int, error) {
	if l.vm.ID != "" && l.vm.State != "released" {
		return []int{l.vm.VMID}, nil
	}
	return nil, nil
}

func (l *liveRunnerLedger) GetRunnerVMOperation(_ context.Context, id string) (store.RunnerVMOperation, error) {
	op, ok := l.ops[id]
	if !ok {
		return store.RunnerVMOperation{}, store.ErrNotFound
	}
	return op, nil
}

func (l *liveRunnerLedger) RecordRunnerVMTerraformPlan(_ context.Context, _, _ string, _ int64,
	id string, evidence store.RunnerVMTerraformPlanEvidence) error {
	op, ok := l.ops[id]
	if !ok || op.Status != "unresolved" {
		return store.ErrConflict
	}
	op.ProviderKind = "terraform"
	op.TerraformVersion = evidence.TerraformVersion
	op.PlanSHA256 = evidence.PlanSHA256
	op.ModuleSHA256 = evidence.ModuleSHA256
	op.InputSHA256 = evidence.InputSHA256
	op.ProviderLockSHA256 = evidence.ProviderLockSHA256
	op.StateIdentitySHA256 = evidence.StateIdentitySHA256
	l.ops[id] = op
	return nil
}

func (l *liveRunnerLedger) BeginRunnerVMTerraformApply(_ context.Context, _, _ string, _ int64,
	id, digest string) (bool, error) {
	op, ok := l.ops[id]
	if !ok || op.ProviderKind != "terraform" || op.PlanSHA256 != digest || op.TerraformApplyStartedAt != nil {
		return false, store.ErrConflict
	}
	now := time.Now().UTC()
	op.TerraformApplyStartedAt = &now
	l.ops[id] = op
	l.vm.CurrentOperationID = id
	l.vm.UnknownOutcome = true
	return true, nil
}

func (l *liveRunnerLedger) ReadRunnerVMTerraformState(context.Context, string) ([]byte, bool, error) {
	return append([]byte(nil), l.state...), len(l.state) != 0, nil
}

func (*liveRunnerLedger) RunnerVMTerraformStateLocked(context.Context, string) (bool, error) {
	return false, nil
}

func (l *liveRunnerLedger) begin(kind string) proxmox.Action {
	id := fmt.Sprintf("0192f3a4-b5c6-7d8e-9f01-%012x", 0x1000+l.opIndex)
	l.opIndex++
	return l.beginAs(id, kind)
}

// beginClone opens the clone under the reservation's creation operation, as
// the production ledger does: the provisioner refuses any other clone action.
func (l *liveRunnerLedger) beginClone() proxmox.Action {
	return l.beginAs(l.vm.CreationOperationID, "clone")
}

func (l *liveRunnerLedger) beginAs(id, kind string) proxmox.Action {
	l.ops[id] = store.RunnerVMOperation{ID: id, RunnerVMID: l.vm.ID,
		Kind: kind, FromState: l.vm.State, PendingState: l.vm.State,
		Status: "unresolved", Generation: l.vm.Generation + 1, ProviderKind: "proxmox"}
	l.vm.CurrentOperationID = id
	l.vm.UnknownOutcome = true
	return proxmox.Action{ID: id}
}

func (l *liveRunnerLedger) settle(action proxmox.Action, result proxmox.Result) error {
	op, ok := l.ops[action.ID]
	if !ok || result.TerraformEvidence == nil ||
		(result.TerraformEvidence.Outcome != "succeeded" && result.TerraformEvidence.Outcome != "failed") {
		return errors.New("lifecycle operation did not reconcile to success")
	}
	op.Status = result.TerraformEvidence.Outcome
	op.ReconciliationSHA256 = result.TerraformEvidence.ReconciliationSHA256
	l.ops[action.ID] = op
	l.vm.CurrentOperationID = ""
	l.vm.UnknownOutcome = false
	l.vm.Generation++
	if result.TerraformEvidence.Outcome == "failed" {
		l.vm.State = op.FromState
		return nil
	}
	switch op.Kind {
	case "clone", "stop":
		l.vm.State = "stopped"
	case "start":
		l.vm.State = "running"
	case "destroy":
		l.vm.State = "released"
	default:
		return errors.New("unknown lifecycle operation")
	}
	return nil
}

func (l *liveRunnerLedger) settleNoEffect(action proxmox.Action) error {
	op, ok := l.ops[action.ID]
	if !ok || op.Status != "unresolved" || op.TerraformApplyStartedAt != nil {
		return store.ErrConflict
	}
	op.Status = "failed"
	l.ops[action.ID] = op
	l.vm.State = op.FromState
	l.vm.CurrentOperationID = ""
	l.vm.UnknownOutcome = false
	l.vm.Generation++
	return nil
}

type liveRunnerRuntime struct {
	inner  *TerraformRuntime
	ledger *liveRunnerLedger
}

func (r *liveRunnerRuntime) runnerConfig() TerraformConfig { return r.inner.runnerConfig() }

func (r *liveRunnerRuntime) WithSession(ctx context.Context, reservationID string,
	callback func(*TerraformSession) error) error {
	return r.inner.WithSession(ctx, reservationID, func(session *TerraformSession) error {
		if err := callback(session); err != nil {
			return err
		}
		state, err := session.StatePull(ctx)
		if err != nil {
			return err
		}
		r.ledger.state = append(r.ledger.state[:0], state...)
		clear(state)
		return nil
	})
}

type liveHTTPState struct {
	mu    sync.Mutex
	state []byte
	lock  string
}

func (state *liveHTTPState) ServeHTTP(writer http.ResponseWriter, request *http.Request) {
	state.mu.Lock()
	defer state.mu.Unlock()
	writer.Header().Set("Content-Type", "application/json")
	switch request.Method {
	case http.MethodGet:
		if len(state.state) == 0 {
			http.NotFound(writer, request)
			return
		}
		_, _ = writer.Write(state.state)
	case http.MethodPost:
		body, err := io.ReadAll(io.LimitReader(request.Body, maxTerraformStateBytes+1))
		if err != nil || len(body) == 0 || len(body) > maxTerraformStateBytes {
			http.Error(writer, "invalid state body", http.StatusRequestEntityTooLarge)
			return
		}
		clear(state.state)
		state.state = append([]byte(nil), body...)
		clear(body)
		writer.WriteHeader(http.StatusOK)
	case "LOCK", "UNLOCK":
		body, err := io.ReadAll(io.LimitReader(request.Body, 64<<10))
		var lock struct{ ID string }
		if err != nil || len(body) == 0 || json.Unmarshal(body, &lock) != nil || lock.ID == "" {
			clear(body)
			http.Error(writer, "invalid lock identity", http.StatusBadRequest)
			return
		}
		if request.Method == "LOCK" {
			if state.lock != "" {
				clear(body)
				http.Error(writer, "state is already locked", http.StatusLocked)
				return
			}
			state.lock = lock.ID
		} else {
			if state.lock == "" || state.lock != lock.ID {
				clear(body)
				http.Error(writer, "state lock does not match", http.StatusConflict)
				return
			}
			state.lock = ""
		}
		_, _ = writer.Write(body)
		clear(body)
	default:
		http.Error(writer, "unsupported state method", http.StatusMethodNotAllowed)
	}
}

func writeLivePrivateFile(t *testing.T, path string, body []byte) {
	t.Helper()
	if err := os.WriteFile(path, body, 0o600); err != nil {
		t.Fatal("write private live-test file")
	}
	if err := os.Chmod(path, 0o600); err != nil {
		t.Fatal("set private live-test file mode")
	}
}

func liveStateBackend(t *testing.T, directory string) HTTPBackendConfig {
	t.Helper()
	now := time.Now().Truncate(time.Second)
	caKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal("generate test state CA key")
	}
	caTemplate := &x509.Certificate{SerialNumber: big.NewInt(1),
		Subject:   pkix.Name{CommonName: "ra8ci live-test state CA"},
		NotBefore: now.Add(-time.Hour), NotAfter: now.Add(48 * time.Hour),
		IsCA: true, BasicConstraintsValid: true,
		KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature}
	caDER, err := x509.CreateCertificate(rand.Reader, caTemplate, caTemplate,
		&caKey.PublicKey, caKey)
	if err != nil {
		t.Fatal("create test state CA")
	}
	caCert, err := x509.ParseCertificate(caDER)
	if err != nil {
		t.Fatal("parse test state CA")
	}
	caPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: caDER})
	caFile := filepath.Join(directory, "state-ca.pem")
	writeLivePrivateFile(t, caFile, caPEM)
	issueLeaf := func(serial int64, commonName string, usage x509.ExtKeyUsage,
		dnsNames []string, ipAddresses []net.IP) (tls.Certificate, []byte, []byte) {
		t.Helper()
		key, keyErr := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if keyErr != nil {
			t.Fatal("generate test state leaf key")
		}
		leafTemplate := &x509.Certificate{SerialNumber: big.NewInt(serial),
			Subject:   pkix.Name{CommonName: commonName},
			NotBefore: now.Add(-time.Hour), NotAfter: now.Add(24 * time.Hour),
			KeyUsage:    x509.KeyUsageDigitalSignature,
			ExtKeyUsage: []x509.ExtKeyUsage{usage}, DNSNames: dnsNames, IPAddresses: ipAddresses}
		leafDER, leafErr := x509.CreateCertificate(rand.Reader, leafTemplate, caCert,
			&key.PublicKey, caKey)
		if leafErr != nil {
			t.Fatal("create test state leaf")
		}
		certificatePEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: leafDER})
		keyDER, keyErr := x509.MarshalPKCS8PrivateKey(key)
		if keyErr != nil {
			t.Fatal("marshal test state leaf key")
		}
		keyPEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
		pair, pairErr := tls.X509KeyPair(certificatePEM, keyPEM)
		if pairErr != nil {
			t.Fatal("load test state leaf")
		}
		return pair, certificatePEM, keyPEM
	}
	_, clientCertificate, clientKey := issueLeaf(2, "ra8ci-terraform-state",
		x509.ExtKeyUsageClientAuth, nil, nil)
	serverPair, _, _ := issueLeaf(3, "localhost", x509.ExtKeyUsageServerAuth,
		[]string{"localhost"}, []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")})
	clientCertFile := filepath.Join(directory, "state-client.pem")
	clientKeyFile := filepath.Join(directory, "state-client-key.pem")
	writeLivePrivateFile(t, clientCertFile, clientCertificate)
	writeLivePrivateFile(t, clientKeyFile, clientKey)
	clientAuthorities := x509.NewCertPool()
	clientAuthorities.AddCert(caCert)
	server := httptest.NewUnstartedServer(&liveHTTPState{})
	server.TLS = &tls.Config{Certificates: []tls.Certificate{serverPair},
		ClientAuth: tls.RequireAndVerifyClientCert, ClientCAs: clientAuthorities,
		MinVersion: tls.VersionTLS12}
	server.StartTLS()
	t.Cleanup(server.Close)
	config := HTTPBackendConfig{ServerURL: server.URL, ReservationID: mustProvisionID(t),
		ClientCertificateFile: clientCertFile, ClientPrivateKeyFile: clientKeyFile,
		ServerCABundleFile: caFile}
	if _, err := HTTPBackendEnvironment(config); err != nil {
		t.Fatalf("generated state backend identity is invalid: %v", err)
	}
	return config
}

func liveOpenBaoAddress(t *testing.T, home string) string {
	t.Helper()
	path := filepath.Join(home, ".config", "hil", "openbao.env")
	addressBytes, err := exec.Command("awk", "-F=", "$1 == \"BAO_ADDR\" {sub(/^[^=]*=/, \"\"); print; exit}", path).Output()
	address := ""
	if err == nil {
		address = strings.TrimSpace(string(addressBytes))
	}
	if address == "" {
		address = strings.TrimSpace(os.Getenv("BAO_ADDR"))
	}
	clear(addressBytes)
	if !strings.HasPrefix(address, "http://") && !strings.HasPrefix(address, "https://") {
		t.Fatal("OpenBao address has a scheme unsupported by the runtime wrapper")
	}
	return address
}

func startLiveProxmoxProxy(t *testing.T, repositoryRoot, directory string) (string, string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	readPublicCertificate := func(path string) *x509.Certificate {
		t.Helper()
		encoded, readErr := exec.CommandContext(ctx, "ssh", "-T", "-o", "BatchMode=yes", "pve",
			"sudo", "-n", "cat", path).Output()
		if readErr != nil {
			t.Fatalf("read %s over the controller SSH path", path)
		}
		block, _ := pem.Decode(encoded)
		clear(encoded)
		if block == nil || block.Type != "CERTIFICATE" {
			t.Fatalf("%s is not a PEM certificate", path)
		}
		parsed, parseErr := x509.ParseCertificate(block.Bytes)
		if parseErr != nil {
			t.Fatalf("parse %s: %v", path, parseErr)
		}
		return parsed
	}
	// The Proxmox client trusts only certificate authorities, so pin the
	// cluster root CA and prove the node certificate chains to it and covers
	// the loopback endpoint the proxy exposes.
	rootCA := readPublicCertificate("/etc/pve/pve-root-ca.pem")
	if !rootCA.IsCA || !rootCA.BasicConstraintsValid {
		t.Fatal("Proxmox cluster root certificate is not a certificate authority")
	}
	nodeCertificate := readPublicCertificate("/etc/pve/local/pve-ssl.pem")
	roots := x509.NewCertPool()
	roots.AddCert(rootCA)
	if _, verifyErr := nodeCertificate.Verify(x509.VerifyOptions{Roots: roots,
		DNSName: "127.0.0.1", KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}); verifyErr != nil {
		t.Fatalf("Proxmox node certificate does not chain to the cluster root for the loopback endpoint: %v", verifyErr)
	}
	caFile := filepath.Join(directory, "proxmox-api-ca.pem")
	writeLivePrivateFile(t, caFile, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: rootCA.Raw}))
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal("reserve a local Proxmox API proxy port")
	}
	port := listener.Addr().(*net.TCPAddr).Port
	_ = listener.Close()
	proxyPath := filepath.Join(repositoryRoot, "scripts", "dev", "ssh_loopback_proxy.py")
	command := exec.Command("python3", proxyPath, "--port", strconv.Itoa(port), "--target", "api")
	command.Stdout = io.Discard
	command.Stderr = io.Discard
	if err := command.Start(); err != nil {
		t.Fatal("start the fixed Proxmox loopback proxy")
	}
	t.Cleanup(func() {
		if command.Process != nil {
			_ = command.Process.Kill()
			_ = command.Wait()
		}
	})
	endpoint := fmt.Sprintf("https://127.0.0.1:%d", port)
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		connection, dialErr := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp",
			fmt.Sprintf("127.0.0.1:%d", port))
		if dialErr == nil {
			tlsConnection := tls.Client(connection, &tls.Config{RootCAs: roots,
				ServerName: "127.0.0.1", MinVersion: tls.VersionTLS12})
			_ = tlsConnection.SetDeadline(time.Now().Add(2 * time.Second))
			handshakeErr := tlsConnection.Handshake()
			_ = tlsConnection.Close()
			if handshakeErr == nil {
				return endpoint, caFile
			}
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatal("Proxmox loopback API proxy did not pass TLS verification")
	return "", ""
}

func createLiveRunnerConfig(t *testing.T, repositoryRoot string) liveRunnerConfig {
	t.Helper()
	directory, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal("resolve live-test directory")
	}
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal("make live-test directory owner-only")
	}
	info, err := os.Stat(directory)
	if err != nil || info.Mode().Perm() != 0o700 {
		t.Fatal("live-test directory is not owner-only")
	}
	home, err := os.UserHomeDir()
	if err != nil {
		t.Fatal("find controller home")
	}
	backend := liveStateBackend(t, directory)
	apiEndpoint, apiCA := startLiveProxmoxProxy(t, repositoryRoot, directory)
	openBaoAddress := liveOpenBaoAddress(t, home)
	wrapperPath := filepath.Join(repositoryRoot, "infra", "terraform", "run-with-openbao.sh")
	terraform := TerraformConfig{BinaryPath: filepath.Join(home, "ra8ci-work", "toolchain",
		"tofu-1.13.0", "tofu"), Version: pinnedOpenTofuVersion,
		CommandWrapper: wrapperPath, WrapperHome: home,
		EnvironmentDirectory: filepath.Join(repositoryRoot, "infra", "terraform", "environments", "ra8ci-runner"),
		StateDirectory:       filepath.Join(directory, "terraform-state"),
		PluginCacheDirectory: filepath.Join(directory, "provider-cache"),
		DiagnosticDirectory:  filepath.Join(directory, "tofu-diagnostics"),
		OperationTimeout:     20 * time.Minute, Backend: backend}
	if err := os.Mkdir(terraform.DiagnosticDirectory, 0o700); err != nil {
		t.Fatalf("create private OpenTofu diagnostic directory: %v", err)
	}
	terraform.BinarySHA256, _ = pinnedOpenTofuBinarySHA256(goruntime.GOOS, goruntime.GOARCH)
	if terraform.BinarySHA256 == "" {
		t.Fatal("controller OpenTofu platform is not pinned")
	}
	proxmoxTokenPath := filepath.Join(directory, "proxmox-api-token")
	t.Cleanup(func() { _ = os.Remove(proxmoxTokenPath) })
	tokenCommand := exec.Command(wrapperPath, "--write-proxmox-token", proxmoxTokenPath)
	tokenCommand.Env = wrapperEnvironment(terraform, nil)
	tokenCommand.Stdout = io.Discard
	var tokenStderr strings.Builder
	tokenCommand.Stderr = &tokenStderr
	if err := tokenCommand.Run(); err != nil {
		t.Fatalf("protected wrapper exited: %v; stderr: %s", err, strings.TrimSpace(tokenStderr.String()))
	}
	if err := privatefile.Check(proxmoxTokenPath); err != nil {
		t.Fatalf("protected wrapper token file failed private-file check: %v", err)
	}
	config := liveRunnerConfig{Terraform: terraform}
	config.Proxmox.Endpoint = apiEndpoint
	config.Proxmox.CAFile = apiCA
	config.Proxmox.TokenFile = proxmoxTokenPath
	config.Runner.Actor = "ra8ci-live-lifecycle-test"
	config.Runner.OpenBaoAddress = openBaoAddress
	config.Runner.OpenBaoKVMount = "secret"
	config.Runner.OpenBaoSecretPath = "terraform/proxmox-lab"
	config.Runner.ModuleDirectory = filepath.Join(repositoryRoot, "infra", "terraform", "modules", "ra8ci_ephemeral_runner")
	configPath := filepath.Join(directory, "runtime-config.json")
	body, err := json.Marshal(config)
	if err != nil {
		t.Fatal("encode generated live-test runtime config")
	}
	writeLivePrivateFile(t, configPath, body)
	clear(body)
	if privatefile.Check(configPath) != nil {
		t.Fatal("generated live-test runtime config is not owner-only")
	}
	encoded, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal("read generated live-test runtime config")
	}
	defer clear(encoded)
	var decoded liveRunnerConfig
	if err := json.Unmarshal(encoded, &decoded); err != nil {
		t.Fatal("decode generated live-test runtime config")
	}
	return decoded
}

func TestLiveTerraformRunnerLifecycle(t *testing.T) {
	if os.Getenv("RA8CI_LAB_LIFECYCLE") != "1" {
		t.Skip("set RA8CI_LAB_LIFECYCLE=1 on the controller to run live lifecycle acceptance")
	}
	_, sourceFile, _, sourceOK := goruntime.Caller(0)
	if !sourceOK {
		t.Fatal("locate live lifecycle test source")
	}
	repositoryRoot := filepath.Clean(filepath.Join(filepath.Dir(sourceFile), "../../../.."))
	config := createLiveRunnerConfig(t, repositoryRoot)
	wrapperPath := filepath.Join(repositoryRoot, "infra", "terraform", "run-with-openbao.sh")
	if config.Terraform.CommandWrapper != wrapperPath {
		t.Fatal("live lifecycle requires the repository OpenBao wrapper")
	}
	wrapperRoot := filepath.Dir(wrapperPath)
	if config.Terraform.EnvironmentDirectory != filepath.Join(wrapperRoot, "environments", "ra8ci-runner") ||
		config.Runner.ModuleDirectory != filepath.Join(wrapperRoot, "modules", "ra8ci_ephemeral_runner") {
		t.Fatal("live lifecycle must use the current ra8ci-runner environment and module")
	}
	operatorHome, err := os.UserHomeDir()
	if err != nil || config.Terraform.WrapperHome != operatorHome {
		t.Fatal("live lifecycle wrapper must use the current controller user's protected Keychain context")
	}

	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Minute)
	defer cancel()
	runtime, err := OpenPinnedTerraformRuntime(ctx, config.Terraform)
	if err != nil {
		t.Fatalf("open pinned OpenTofu runtime through the OpenBao wrapper: %v", err)
	}
	allowedIDs := make([]int, 0, 19)
	for vmid := 9020; vmid <= 9039; vmid++ {
		if vmid != windowsLifecycleVMID {
			allowedIDs = append(allowedIDs, vmid)
		}
	}
	client, err := proxmox.New(proxmox.Config{Endpoint: config.Proxmox.Endpoint,
		CAFile: config.Proxmox.CAFile, TokenFile: config.Proxmox.TokenFile,
		Node: "pve1", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: allowedIDs, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr9"},
		OperationTimeout: 10 * time.Minute})
	if err != nil {
		t.Fatalf("open constrained Proxmox client: %v", err)
	}
	if err := os.Remove(config.Proxmox.TokenFile); err != nil {
		t.Fatal("remove temporary Proxmox token file after client initialization")
	}

	ledger := &liveRunnerLedger{ops: make(map[string]store.RunnerVMOperation)}
	profiles := make(map[int]TerraformRunnerProfile, 19)
	for vmid := 9020; vmid <= 9039; vmid++ {
		if vmid == windowsLifecycleVMID {
			continue
		}
		profiles[vmid] = TerraformRunnerProfile{
			TemplateVMID: 9001, TemplateName: "ra8-lab-debian-template",
			Node: "pve1", Pool: "ra8-tf-lab", DatastoreID: "ra8-tf-lab", Bridge: "vmbr9",
			Cores: 4, MemoryMB: 8192, IPv4Address: fmt.Sprintf("10.250.9.%d/24", vmid-8970),
			IPv4Gateway: "10.250.9.1", UserName: "ra8ci",
		}
	}
	keys, err := NewSSHAccessStore(filepath.Join(config.Terraform.StateDirectory, "live-ssh-keys"))
	if err != nil {
		t.Fatal("prepare private runner key store")
	}
	provisioner, err := NewTerraformRunnerProvisioner(&liveRunnerRuntime{inner: runtime, ledger: ledger},
		ledger, client, keys, TerraformRunnerConfig{Actor: config.Runner.Actor,
			ProxmoxEndpoint: config.Proxmox.Endpoint, OpenBaoAddress: config.Runner.OpenBaoAddress,
			OpenBaoKVMount: config.Runner.OpenBaoKVMount, OpenBaoSecretPath: config.Runner.OpenBaoSecretPath,
			Profiles: profiles, ModuleDirectory: config.Runner.ModuleDirectory})
	if err != nil {
		t.Fatalf("construct constrained Terraform runner provisioner: %v", err)
	}
	input := store.RunnerVMInput{ScaleSetID: 1, JobID: "live-lifecycle-acceptance",
		RunnerRequestID: 1, WorkflowRunID: time.Now().Unix(), WorkflowAttempt: 1,
		Repository: "owner/repo", WorkflowRef: "refs/heads/dev",
		CommitSHA: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
	vm, created, err := provisioner.Reserve(ctx, input, time.Now().Add(time.Hour))
	if err != nil || !created {
		t.Fatal("reserve live runner VMID")
	}
	runID := fmt.Sprintf("%016x", uint64(vm.WorkflowRunID))
	if output, err := runLiveLabNetworkAction(ctx, repositoryRoot, "up", runID, vm.VMID); err != nil {
		t.Fatalf("bring up the live test's reviewed lab network: %v: %s", err, output)
	} else {
		t.Log(output)
	}
	t.Cleanup(func() {
		cleanupCtx, cleanupCancel := context.WithTimeout(context.Background(), 5*time.Minute)
		defer cleanupCancel()
		output, err := runLiveLabNetworkAction(cleanupCtx, repositoryRoot, "down", runID, vm.VMID)
		if err != nil {
			t.Errorf("live lifecycle network cleanup failed: %v: %s", err, output)
			return
		}
		t.Log(output)
	})
	identity := proxmox.Identity{VMID: vm.VMID, Node: vm.Node, Pool: vm.Pool, Storage: vm.Storage,
		Name: vm.Name, ReservationID: vm.ID, CreationOperationID: vm.CreationOperationID,
		RunID: runID}
	t.Cleanup(func() {
		if err := cleanupLiveRunner(provisioner, client, ledger, identity); err != nil {
			t.Errorf("live lifecycle cleanup needs operator attention: %v", err)
		}
	})
	profile := profiles[vm.VMID]

	clone := ledger.beginClone()
	cloneResult, err := provisioner.Clone(ctx, clone, proxmox.CloneSpec{Target: identity,
		TemplateVMID: 9001, TemplateName: vm.TemplateName, TemplateDigest: vm.TemplateDigest})
	if err != nil {
		t.Fatalf("clone runner guest: %v%s", err, liveTerraformDiagnostics(config.Terraform.DiagnosticDirectory))
	}
	if settleErr := ledger.settle(clone, cloneResult); settleErr != nil {
		t.Fatalf("reconcile runner guest after clone: %v", settleErr)
	}
	cloned, err := client.Get(ctx, identity)
	if err != nil || cloned.Protected {
		t.Fatalf("verify clone is unprotected: protected=%t err=%v", cloned.Protected, err)
	}
	t.Logf("clone unprotected: VMID %d has protection disabled", identity.VMID)
	start := ledger.begin("start")
	startResult, err := provisioner.Start(ctx, start, identity)
	if err != nil {
		t.Fatalf("start runner guest: %v%s", err, liveTerraformDiagnostics(config.Terraform.DiagnosticDirectory))
	}
	if settleErr := ledger.settle(start, startResult); settleErr != nil {
		t.Fatalf("reconcile runner guest after start: %v", settleErr)
	}
	probeRunnerSSH(ctx, t, profile.IPv4Address)
	idle := proxmox.IdleProof{VMID: vm.VMID, ReservationID: vm.ID,
		EvidenceID: "0192f3a4-b5c6-7d8e-9f01-1234567890b1", ObservedAt: time.Now(),
		Drained: true, NoActiveJob: true}
	stop := ledger.begin("stop")
	stopResult, err := provisioner.Stop(ctx, stop, identity, idle)
	if err != nil {
		t.Fatalf("stop runner guest: %v%s", err, liveTerraformDiagnostics(config.Terraform.DiagnosticDirectory))
	}
	if settleErr := ledger.settle(stop, stopResult); settleErr != nil {
		t.Fatalf("reconcile runner guest after stop: %v", settleErr)
	}
	observed, err := client.Get(ctx, identity)
	if err != nil {
		t.Fatalf("observe stopped runner guest before destroy: %v", err)
	}
	destroy := ledger.begin("destroy")
	destroyResult, err := provisioner.Destroy(ctx, destroy, identity, proxmox.DestroyProof{
		IdleProof: liveIdleProof(identity), ApprovalID: "0192f3a4-b5c6-7d8e-9f01-1234567890b2",
		ExpectedConfigDigest: observed.ConfigDigest, RunnerDeregistered: true, StateReconciled: true,
	})
	if err != nil {
		t.Fatalf("destroy runner guest: %v%s", err, liveTerraformDiagnostics(config.Terraform.DiagnosticDirectory))
	}
	if settleErr := ledger.settle(destroy, destroyResult); settleErr != nil {
		t.Fatalf("reconcile runner guest after destroy: %v", settleErr)
	}
	if _, err := client.Get(ctx, identity); !errors.Is(err, proxmox.ErrNotFound) {
		t.Fatalf("independent Proxmox read after destroy did not report not-found: %v", err)
	}
	occupied, err := client.OccupiedVMIDs(ctx)
	if err != nil {
		t.Fatalf("independently list Proxmox VMIDs after destroy: %v", err)
	}
	for _, vmid := range occupied {
		if vmid == identity.VMID {
			t.Fatal("independent Proxmox inventory still contains the runner VMID")
		}
	}
	t.Logf("guest destroyed: VMID %d is absent from Proxmox inventory", identity.VMID)
	t.Logf("live lifecycle passed: VMID %d cloned, started, boot-probed, stopped, destroyed, reconciled, and absent", identity.VMID)
}

func runLiveLabNetworkAction(ctx context.Context, repositoryRoot, action, runID string, vmid int) (string, error) {
	script := filepath.Join(repositoryRoot, "infra", "terraform", "lab-guest.sh")
	cmd := exec.CommandContext(ctx, script, "network", action, runID)
	env := make([]string, 0, len(os.Environ())+2)
	for _, item := range os.Environ() {
		key, _, _ := strings.Cut(item, "=")
		if key == "RA8_TOFU_GUEST_PROFILE" || key == "RA8_TOFU_GUEST_VM_ID" {
			continue
		}
		env = append(env, item)
	}
	env = append(env, "RA8_TOFU_GUEST_PROFILE=linux", "RA8_TOFU_GUEST_VM_ID="+strconv.Itoa(vmid))
	cmd.Env = env
	output, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(output)), err
}

func probeRunnerSSH(ctx context.Context, t *testing.T, prefix string) {
	t.Helper()
	network, err := netip.ParsePrefix(prefix)
	if err != nil {
		t.Fatal("invalid reviewed runner SSH address")
	}
	address := net.JoinHostPort(network.Addr().String(), "22")
	deadline := time.Now().Add(4 * time.Minute)
	for time.Now().Before(deadline) {
		probeCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
		connection, dialErr := (&net.Dialer{}).DialContext(probeCtx, "tcp", address)
		cancel()
		if dialErr == nil {
			_ = connection.Close()
			return
		}
		time.Sleep(2 * time.Second)
	}
	t.Fatal("runner did not pass the bounded SSH boot probe")
}

func cleanupLiveRunner(provisioner *TerraformRunnerProvisioner, client *proxmox.Client,
	ledger *liveRunnerLedger, identity proxmox.Identity) error {
	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Minute)
	defer cancel()
	observed, err := client.Get(ctx, identity)
	if errors.Is(err, proxmox.ErrNotFound) {
		return nil
	}
	if err != nil {
		return errors.New("cannot independently observe the runner guest")
	}
	if observed.Locked || observed.Protected {
		return errors.New("runner guest is locked or protected; cleanup refused")
	}
	if ledger.vm.UnknownOutcome && ledger.vm.CurrentOperationID != "" {
		op, getErr := ledger.GetRunnerVMOperation(ctx, ledger.vm.CurrentOperationID)
		if getErr != nil {
			return errors.New("cannot read unresolved lifecycle operation")
		}
		result, reconcileErr := provisioner.Reconcile(ctx, op.ID, identity, op.Kind, "")
		if reconcileErr != nil {
			return errors.New("cannot reconcile unresolved lifecycle operation")
		}
		if result.TerraformEvidence != nil {
			if ledger.settle(proxmox.Action{ID: op.ID}, result) != nil {
				return errors.New("cannot settle reconciled lifecycle operation")
			}
		} else if result.TerraformPreflightNoEffect != nil {
			if ledger.settleNoEffect(proxmox.Action{ID: op.ID}) != nil {
				return errors.New("cannot settle lifecycle operation with no effect")
			}
		} else {
			return errors.New("lifecycle operation has no safe reconciliation evidence")
		}
		observed, err = client.Get(ctx, identity)
		if errors.Is(err, proxmox.ErrNotFound) {
			return nil
		}
		if err != nil || observed.Locked || observed.Protected {
			return errors.New("runner guest remains unavailable, locked, or protected after reconciliation")
		}
	}
	if observed.Status == "running" {
		stop := ledger.begin("stop")
		result, stopErr := provisioner.Stop(ctx, stop, identity, liveIdleProof(identity))
		if stopErr != nil || ledger.settle(stop, result) != nil {
			return errors.New("could not safely stop runner guest during cleanup")
		}
		observed, err = client.Get(ctx, identity)
		if err != nil {
			return errors.New("could not verify stopped runner guest during cleanup")
		}
	}
	if observed.Status != "stopped" {
		return errors.New("runner guest is not stopped; cleanup refused")
	}
	destroy := ledger.begin("destroy")
	result, destroyErr := provisioner.Destroy(ctx, destroy, identity, proxmox.DestroyProof{
		IdleProof: liveIdleProof(identity), ApprovalID: "0192f3a4-b5c6-7d8e-9f01-1234567890b3",
		ExpectedConfigDigest: observed.ConfigDigest, RunnerDeregistered: true, StateReconciled: true,
	})
	if destroyErr != nil || ledger.settle(destroy, result) != nil {
		return errors.New("could not destroy runner guest during cleanup")
	}
	if _, err := client.Get(ctx, identity); !errors.Is(err, proxmox.ErrNotFound) {
		return errors.New("independent Proxmox read still finds runner guest after cleanup")
	}
	return nil
}

func liveIdleProof(identity proxmox.Identity) proxmox.IdleProof {
	return proxmox.IdleProof{VMID: identity.VMID, ReservationID: identity.ReservationID,
		EvidenceID: "0192f3a4-b5c6-7d8e-9f01-1234567890b4", ObservedAt: time.Now(),
		Drained: true, NoActiveJob: true}
}

var liveDiagnosticSecret = regexp.MustCompile(`(PVEAPIToken=\S+|hvs\.[A-Za-z0-9_-]+|s\.[A-Za-z0-9]{24,}|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}=[0-9a-f-]+)`)

// liveTerraformDiagnostics returns the Error blocks from OpenTofu stderr
// files the runtime saved, at most 40 lines, with token-shaped values
// redacted, so an operator sees why a lifecycle command failed.
func liveTerraformDiagnostics(directory string) string {
	paths, _ := filepath.Glob(filepath.Join(directory, "*.stderr"))
	if len(paths) == 0 {
		return ""
	}
	var lines []string
	for _, path := range paths {
		data, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		inError := false
		for _, line := range strings.Split(string(data), "\n") {
			if strings.Contains(line, "Error:") {
				inError = true
			}
			if inError && len(lines) < 40 {
				lines = append(lines, liveDiagnosticSecret.ReplaceAllString(line, "[redacted]"))
			}
		}
		clear(data)
	}
	if len(lines) == 0 {
		return "\nOpenTofu stderr held no Error block"
	}
	return "\nOpenTofu stderr:\n" + strings.Join(lines, "\n")
}

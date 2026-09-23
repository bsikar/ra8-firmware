package github

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v4"
)

type metadataTestTransport struct {
	destination *url.URL
	transport   http.RoundTripper
}

func (t metadataTestTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	rewritten := request.Clone(request.Context())
	urlCopy := *rewritten.URL
	urlCopy.Scheme, urlCopy.Host = t.destination.Scheme, t.destination.Host
	rewritten.URL, rewritten.Host = &urlCopy, t.destination.Host
	return t.transport.RoundTrip(rewritten)
}

func TestMetadataResolverScopesTokenAndBindsWorkflowRun(t *testing.T) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	if err := os.WriteFile(keyPath, keyPEM, 0600); err != nil {
		t.Fatal(err)
	}
	defer clear(keyPEM)

	tokenCalls, runCalls := 0, 0
	runBody := workflowRunResponse{ID: 1234, RunAttempt: 2, HeadSHA: strings.Repeat("a", 40), HeadBranch: "dev", Path: ".github/workflows/ci.yml@dev", Event: "push"}
	runBody.Repository.FullName = "bsikar/ra8-firmware"
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/app/installations/42/access_tokens":
			tokenCalls++
			var body installationTokenRequest
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
				t.Error(err)
			}
			if r.Method != http.MethodPost || len(body.Repositories) != 1 || body.Repositories[0] != "ra8-firmware" || body.Permissions["actions"] != "read" || len(body.Permissions) != 1 {
				t.Errorf("token scope/request unexpected: %+v", body)
			}
			bearer := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
			claims := &jwt.RegisteredClaims{}
			parsed, err := jwt.ParseWithClaims(bearer, claims, func(token *jwt.Token) (any, error) { return &key.PublicKey, nil })
			if err != nil || parsed == nil || !parsed.Valid || claims.Issuer != "client-id" {
				t.Errorf("invalid App JWT: %v", err)
			}
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour)})
		case "/repos/bsikar/ra8-firmware/actions/runs/1234":
			runCalls++
			if r.Method != http.MethodGet || r.Header.Get("Authorization") != "Bearer installation-token" {
				t.Errorf("workflow request not authorized")
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(runBody)
		case "/repos/bsikar/ra8-firmware/actions/runs/1234/attempts/2/jobs":
			if r.Method != http.MethodGet || r.URL.Query().Get("per_page") != "100" || r.URL.Query().Get("page") != "1" {
				t.Errorf("workflow attempt jobs request malformed: %s?%s", r.URL.Path, r.URL.RawQuery)
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"total_count": 1, "jobs": []map[string]any{{
				"id": 91, "run_id": 1234, "name": "build", "head_sha": strings.Repeat("a", 40), "head_branch": "dev",
			}}})
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	destination, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	client := server.Client()
	client.Transport = metadataTestTransport{destination: destination, transport: client.Transport}
	r, err := NewMetadataResolver(MetadataConfig{APIBaseURL: "https://api.github.com", AppClientID: "client-id", InstallationID: 42,
		PrivateKeyFile: keyPath, Owner: "bsikar", Repository: "ra8-firmware", httpClient: client})
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("HTTP_PROXY", "http://127.0.0.1:8080")
	t.Setenv("HTTPS_PROXY", "http://127.0.0.1:8080")
	direct, err := NewMetadataResolver(MetadataConfig{AppClientID: "client-id", InstallationID: 42,
		PrivateKeyFile: keyPath, Owner: "bsikar", Repository: "ra8-firmware"})
	if err != nil {
		t.Fatal(err)
	}
	transport, ok := direct.client.Transport.(*http.Transport)
	if !ok || transport.Proxy != nil {
		t.Fatal("default GitHub metadata transport inherited a proxy")
	}
	job := Job{Owner: "bsikar", Repository: "ra8-firmware", JobID: "job-guid", WorkflowRunID: 1234,
		WorkflowRef: "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev", DisplayName: "build", EventName: "push"}
	got, err := r.Resolve(context.Background(), job)
	if err != nil {
		t.Fatal(err)
	}
	if got.WorkflowAttempt != 2 || got.CommitSHA != strings.Repeat("a", 40) || got.JobID != job.JobID || got.Repository != "bsikar/ra8-firmware" {
		t.Fatalf("incorrect metadata: %+v", got)
	}
	if _, err := r.Resolve(context.Background(), job); err != nil {
		t.Fatal(err)
	}
	if tokenCalls != 1 || runCalls != 2 {
		t.Fatalf("token calls=%d run calls=%d", tokenCalls, runCalls)
	}
	job.DisplayName = "not-the-job"
	if _, err := r.Resolve(context.Background(), job); err == nil {
		t.Fatal("workflow run metadata accepted a job absent from the exact attempt")
	}
	job.DisplayName = "build"
	runBody.HeadBranch = "main"
	if _, err := r.Resolve(context.Background(), job); err == nil {
		t.Fatal("mismatched branch accepted")
	}
	job.WorkflowRef = "bsikar/ra8-firmware/.github/workflows/../evil.yml@refs/heads/dev"
	if _, err := r.Resolve(context.Background(), job); err == nil {
		t.Fatal("workflow path traversal accepted")
	}
}

func TestMetadataResolverRejectsUnsafeURLAndNonBranchRef(t *testing.T) {
	for _, apiURL := range []string{"http://api.github.com", "https://user:pass@api.github.com", "https://api.github.com?x=y",
		"https://evil.example", "https://api.github.com:8443", "https://api.github.com/api/v3",
	} {
		if _, err := NewMetadataResolver(MetadataConfig{APIBaseURL: apiURL}); err == nil {
			t.Errorf("unsafe URL accepted: %s", apiURL)
		}
	}
	if _, _, ok := parseWorkflowRef("bsikar/ra8-firmware/.github/workflows/ci.yml@refs/pull/12/merge", "bsikar", "ra8-firmware"); ok {
		t.Fatal("pull request ref accepted by branch-only metadata resolver")
	}
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"math/big"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
)

// shapedBackendConfig mints a CA, a client leaf the caller may shape, and the
// three files HTTPBackendEnvironment reads, then hands back a configuration
// that is otherwise the reviewed one. Shaping the leaf is how an identity
// that parses as a key pair and is still refused by the identity rule gets
// built.
func shapedBackendConfig(t *testing.T, shapeLeaf func(*x509.Certificate)) HTTPBackendConfig {
	t.Helper()
	directory := t.TempDir()
	now := time.Now()
	caKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	caTemplate := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "ra8ci refusal CA"},
		NotBefore:             now.Add(-time.Hour),
		NotAfter:              now.Add(24 * time.Hour),
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
	}
	caDER, err := x509.CreateCertificate(rand.Reader, caTemplate, caTemplate, &caKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	clientKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	leaf := &x509.Certificate{
		SerialNumber: big.NewInt(2),
		Subject:      pkix.Name{CommonName: "ra8ci-terraform-state"},
		NotBefore:    now.Add(-time.Hour),
		NotAfter:     now.Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}
	if shapeLeaf != nil {
		shapeLeaf(leaf)
	}
	clientDER, err := x509.CreateCertificate(rand.Reader, leaf, caTemplate, &clientKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(clientKey)
	if err != nil {
		t.Fatal(err)
	}
	certPath := filepath.Join(directory, "client.pem")
	keyPath := filepath.Join(directory, "client-key.pem")
	caPath := filepath.Join(directory, "ca.pem")
	writeBackendPEM(t, certPath, "CERTIFICATE", clientDER, 0o644)
	writeBackendPEM(t, keyPath, "PRIVATE KEY", keyDER, 0o600)
	writeBackendPEM(t, caPath, "CERTIFICATE", caDER, 0o644)
	return HTTPBackendConfig{
		ServerURL:             "https://ra8ci.internal.example:8443/api",
		ReservationID:         mustProvisionID(t),
		ClientCertificateFile: certPath,
		ClientPrivateKeyFile:  keyPath,
		ServerCABundleFile:    caPath,
	}
}

func writeBackendPEM(t *testing.T, file, kind string, der []byte, mode os.FileMode) {
	t.Helper()
	body := pem.EncodeToMemory(&pem.Block{Type: kind, Bytes: der})
	if err := os.WriteFile(file, body, mode); err != nil {
		t.Fatal(err)
	}
}

// refusedBackend asserts the environment was not built and hands back the
// refusal. Nothing may be returned beside an error: every entry this function
// builds carries either a state URL or PEM material.
func refusedBackend(t *testing.T, config HTTPBackendConfig) error {
	t.Helper()
	environment, err := HTTPBackendEnvironment(config)
	if err == nil {
		t.Fatal("accepted a Terraform HTTP backend configuration that should have been refused")
	}
	if environment != nil {
		t.Fatalf("refused configuration still produced %d environment entries", len(environment))
	}
	return err
}

func mustContain(t *testing.T, err error, want string) {
	t.Helper()
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("refusal %q does not name %q", err.Error(), want)
	}
}

// TestBackendConfigurationIsRefusedBeforeAnythingIsOpened pins the cheapest
// guard in the file. It reads only the configuration struct, so a malformed
// deployment is refused without a parse, a file read, or a private key ever
// reaching memory, and every one of these shapes reads the same way.
func TestBackendConfigurationIsRefusedBeforeAnythingIsOpened(t *testing.T) {
	valid := mustProvisionID(t)
	v4 := valid[:14] + "4" + valid[15:]
	badVariant := valid[:19] + "c" + valid[20:]
	for name, mutate := range map[string]func(*HTTPBackendConfig){
		"no server URL":            func(c *HTTPBackendConfig) { c.ServerURL = "" },
		"server URL with a space":  func(c *HTTPBackendConfig) { c.ServerURL = " " + c.ServerURL },
		"server URL with a tail":   func(c *HTTPBackendConfig) { c.ServerURL += "\n" },
		"no reservation":           func(c *HTTPBackendConfig) { c.ReservationID = "" },
		"reservation not a UUID":   func(c *HTTPBackendConfig) { c.ReservationID = "runner-7" },
		"reservation version four": func(c *HTTPBackendConfig) { c.ReservationID = v4 },
		"reservation variant":      func(c *HTTPBackendConfig) { c.ReservationID = badVariant },
		"reservation uppercase":    func(c *HTTPBackendConfig) { c.ReservationID = strings.ToUpper(valid) },
		"no client certificate":    func(c *HTTPBackendConfig) { c.ClientCertificateFile = "" },
		"no client private key":    func(c *HTTPBackendConfig) { c.ClientPrivateKeyFile = "" },
		"no server CA bundle":      func(c *HTTPBackendConfig) { c.ServerCABundleFile = "" },
	} {
		t.Run(name, func(t *testing.T) {
			config := shapedBackendConfig(t, nil)
			mutate(&config)
			err := refusedBackend(t, config)
			if err.Error() != "invalid Terraform HTTP backend configuration" {
				t.Fatalf("refusal %q is not the configuration refusal", err.Error())
			}
		})
	}
}

// TestTheConfigurationGuardRunsAheadOfTheOrigin pins the order between the
// first two refusals. A deployment wrong in both ways is told about its own
// configuration rather than about an endpoint it never meant to reach.
func TestTheConfigurationGuardRunsAheadOfTheOrigin(t *testing.T) {
	config := shapedBackendConfig(t, nil)
	config.ServerURL = "http://ra8ci.internal.example:8443"
	config.ReservationID = ""
	err := refusedBackend(t, config)
	if err.Error() != "invalid Terraform HTTP backend configuration" {
		t.Fatalf("refusal %q is not the configuration refusal", err.Error())
	}
}

// TestTheOriginIsJudgedBeforeAnyCredentialIsRead pins the other half of that
// order: an unapproved endpoint is refused while the credential files are
// still missing, so no private key is read for a destination that was never
// going to be used.
func TestTheOriginIsJudgedBeforeAnyCredentialIsRead(t *testing.T) {
	for endpoint, want := range map[string]string{
		"http://ra8ci.internal.example:8443":  "Terraform state backend requires an explicit HTTPS origin",
		"https://ra8ci.internal.example":      "Terraform state backend requires an explicit HTTPS origin",
		"https://control.tail123.ts.net:8443": "personal-network Terraform endpoint is prohibited",
		"https://100.64.1.2:8443":             "personal-network Terraform endpoint is prohibited",
	} {
		t.Run(endpoint, func(t *testing.T) {
			config := shapedBackendConfig(t, nil)
			config.ServerURL = endpoint
			missing := filepath.Join(t.TempDir(), "absent.pem")
			config.ClientCertificateFile = missing
			config.ClientPrivateKeyFile = missing
			config.ServerCABundleFile = missing
			if got := refusedBackend(t, config).Error(); got != want {
				t.Fatalf("refusal %q want %q", got, want)
			}
		})
	}
}

// TestEachCredentialFileIsNamedInItsOwnRefusal pins which file an operator is
// sent to look at. The three reads share one reason underneath, so without
// the wrapping every missing-file refusal would read identically.
func TestEachCredentialFileIsNamedInItsOwnRefusal(t *testing.T) {
	for name, pick := range map[string]func(*HTTPBackendConfig) *string{
		"read Terraform client certificate": func(c *HTTPBackendConfig) *string { return &c.ClientCertificateFile },
		"read Terraform client private key": func(c *HTTPBackendConfig) *string { return &c.ClientPrivateKeyFile },
		"read Terraform server CA bundle":   func(c *HTTPBackendConfig) *string { return &c.ServerCABundleFile },
	} {
		t.Run(name, func(t *testing.T) {
			config := shapedBackendConfig(t, nil)
			field := pick(&config)
			if err := os.Remove(*field); err != nil {
				t.Fatal(err)
			}
			err := refusedBackend(t, config)
			mustContain(t, err, name)
			mustContain(t, err, "credential must be a bounded regular file")
		})
	}
}

// TestCredentialFilesMustBeBoundedRegularFiles pins what a credential file is
// allowed to be. The bound is on the bytes and is exact: a certificate of
// exactly the ceiling is read and refused later for what it contains, one
// byte more is refused for its size and never read at all.
func TestCredentialFilesMustBeBoundedRegularFiles(t *testing.T) {
	t.Run("empty", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		if err := os.WriteFile(config.ClientCertificateFile, nil, 0o644); err != nil {
			t.Fatal(err)
		}
		mustContain(t, refusedBackend(t, config), "credential must be a bounded regular file")
	})
	t.Run("a directory", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		directory := filepath.Join(t.TempDir(), "certs")
		if err := os.Mkdir(directory, 0o755); err != nil {
			t.Fatal(err)
		}
		config.ClientCertificateFile = directory
		mustContain(t, refusedBackend(t, config), "credential must be a bounded regular file")
	})
	t.Run("a symlink to the real certificate", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		link := filepath.Join(t.TempDir(), "client-link.pem")
		if err := os.Symlink(config.ClientCertificateFile, link); err != nil {
			t.Fatal(err)
		}
		config.ClientCertificateFile = link
		mustContain(t, refusedBackend(t, config), "credential must be a bounded regular file")
	})
	t.Run("exactly at the ceiling", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		body := []byte(strings.Repeat("a", maxClientCertificateBytes))
		if err := os.WriteFile(config.ClientCertificateFile, body, 0o644); err != nil {
			t.Fatal(err)
		}
		err := refusedBackend(t, config)
		if err.Error() != "Terraform client certificate and private key do not match" {
			t.Fatalf("a certificate at the ceiling was not read: %q", err.Error())
		}
	})
	t.Run("one byte past the ceiling", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		body := []byte(strings.Repeat("a", maxClientCertificateBytes+1))
		if err := os.WriteFile(config.ClientCertificateFile, body, 0o644); err != nil {
			t.Fatal(err)
		}
		mustContain(t, refusedBackend(t, config), "credential must be a bounded regular file")
	})
	t.Run("unreadable", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		if err := os.Chmod(config.ClientCertificateFile, 0o000); err != nil {
			t.Fatal(err)
		}
		mustContain(t, refusedBackend(t, config), "credential file is unreadable")
	})
}

// TestThePrivateKeyMayNotBeReadableByAnyoneElse pins the one rule that is
// asked of the key file and of nothing else. Group and other are both
// refused, and the mode is read from the file rather than from the handle's
// owner, so a key left world-readable on the service disk never reaches a
// Terraform child.
func TestThePrivateKeyMayNotBeReadableByAnyoneElse(t *testing.T) {
	for _, mode := range []os.FileMode{0o640, 0o604, 0o660, 0o606, 0o644} {
		t.Run(mode.String(), func(t *testing.T) {
			config := shapedBackendConfig(t, nil)
			if err := os.Chmod(config.ClientPrivateKeyFile, mode); err != nil {
				t.Fatal(err)
			}
			err := refusedBackend(t, config)
			mustContain(t, err, "read Terraform client private key")
			mustContain(t, err, "private key file must not be accessible by group or others")
		})
	}
	config := shapedBackendConfig(t, nil)
	if err := os.Chmod(config.ClientPrivateKeyFile, 0o400); err != nil {
		t.Fatal(err)
	}
	if _, err := HTTPBackendEnvironment(config); err != nil {
		t.Fatalf("refused an owner-only private key: %v", err)
	}
}

// TestTheCertificateAndKeyMustBeOnePair pins that the two files are checked
// against each other rather than merely parsed. A certificate paired with
// another reservation's key is the shape a half-finished rotation leaves
// behind, and it fails at the handshake rather than here if nothing asks.
func TestTheCertificateAndKeyMustBeOnePair(t *testing.T) {
	t.Run("a key from another pair", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		other := shapedBackendConfig(t, nil)
		config.ClientPrivateKeyFile = other.ClientPrivateKeyFile
		err := refusedBackend(t, config)
		if err.Error() != "Terraform client certificate and private key do not match" {
			t.Fatalf("refusal %q is not the mismatch refusal", err.Error())
		}
	})
	t.Run("a certificate that is not PEM", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		if err := os.WriteFile(config.ClientCertificateFile, []byte("not a certificate"), 0o644); err != nil {
			t.Fatal(err)
		}
		err := refusedBackend(t, config)
		if err.Error() != "Terraform client certificate and private key do not match" {
			t.Fatalf("refusal %q is not the mismatch refusal", err.Error())
		}
	})
	t.Run("the CA certificate in the client slot", func(t *testing.T) {
		config := shapedBackendConfig(t, nil)
		bundle, err := os.ReadFile(config.ServerCABundleFile)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(config.ClientCertificateFile, bundle, 0o644); err != nil {
			t.Fatal(err)
		}
		refused := refusedBackend(t, config)
		if refused.Error() != "Terraform client certificate and private key do not match" {
			t.Fatalf("refusal %q is not the mismatch refusal", refused.Error())
		}
	})
}

// TestTheIdentityRefusalIsPassedThroughVerbatim pins that this function does
// not reword the one client-identity rule. The plane has a single rule,
// checkTerraformStateClientIdentity over mtls.ValidateClientIdentity, and a
// caller matching on mtls.ErrIdentity must be able to classify what happened
// here without reading message text.
func TestTheIdentityRefusalIsPassedThroughVerbatim(t *testing.T) {
	now := time.Now()
	for name, shape := range map[string]func(*x509.Certificate){
		"expired": func(leaf *x509.Certificate) {
			leaf.NotBefore = now.Add(-48 * time.Hour)
			leaf.NotAfter = now.Add(-time.Hour)
		},
		"not yet valid": func(leaf *x509.Certificate) {
			leaf.NotBefore = now.Add(time.Hour)
			leaf.NotAfter = now.Add(2 * time.Hour)
		},
		"a certificate authority": func(leaf *x509.Certificate) {
			leaf.IsCA = true
			leaf.BasicConstraintsValid = true
			leaf.KeyUsage |= x509.KeyUsageCertSign
		},
		"server authentication only": func(leaf *x509.Certificate) {
			leaf.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
		},
	} {
		t.Run(name, func(t *testing.T) {
			config := shapedBackendConfig(t, shape)
			err := refusedBackend(t, config)
			if !errors.Is(err, mtls.ErrIdentity) {
				t.Fatalf("refusal %q is not classifiable as an identity problem", err.Error())
			}
			pair, loadErr := tls.LoadX509KeyPair(config.ClientCertificateFile, config.ClientPrivateKeyFile)
			if loadErr != nil {
				t.Fatal(loadErr)
			}
			want := checkTerraformStateClientIdentity(pair, time.Now())
			if want == nil {
				t.Fatal("the identity rule admitted a leaf the backend refused")
			}
			if err.Error() != want.Error() {
				t.Fatalf("backend reworded the identity refusal:\n got %q\nwant %q", err.Error(), want.Error())
			}
		})
	}
}

// TestTheClientIdentityIsJudgedBeforeTheServerBundle pins the last ordering in
// the function. A deployment whose client certificate expired overnight is
// told that, not that its CA bundle is missing, which is the difference
// between rotating a certificate and going to look at the wrong file.
func TestTheClientIdentityIsJudgedBeforeTheServerBundle(t *testing.T) {
	config := shapedBackendConfig(t, func(leaf *x509.Certificate) {
		leaf.NotBefore = time.Now().Add(-48 * time.Hour)
		leaf.NotAfter = time.Now().Add(-time.Hour)
	})
	if err := os.Remove(config.ServerCABundleFile); err != nil {
		t.Fatal(err)
	}
	err := refusedBackend(t, config)
	mustContain(t, err, "not a currently valid client leaf")
	if strings.Contains(err.Error(), "server CA bundle") {
		t.Fatalf("refusal %q named the CA bundle ahead of the client identity", err.Error())
	}
}

// TestTheOverlayRefusesMalformedBaseEntries pins the base half of the scrub.
// The base is this process's own environment, so an entry that is not a
// variable at all means the caller handed over something other than an
// environment, and the scrub refuses rather than carrying it.
func TestTheOverlayRefusesMalformedBaseEntries(t *testing.T) {
	for _, entry := range []string{
		"PATH",
		"=/usr/bin",
		"1PATH=/usr/bin",
		"TF-DATA-DIR=/safe",
		"PATH /usr/bin",
		"TF_DATA DIR=/safe",
	} {
		t.Run(entry, func(t *testing.T) {
			result, err := OverlayEnvironment([]string{entry}, nil)
			if err == nil {
				t.Fatalf("accepted base entry %q", entry)
			}
			if result != nil {
				t.Fatalf("refused base entry %q still produced an environment", entry)
			}
			if err.Error() != "invalid base environment entry" {
				t.Fatalf("refusal %q is not the base entry refusal", err.Error())
			}
		})
	}
}

// TestTheOverlayBoundsAShortLivedVaultToken pins the one overlay value that
// is judged on its contents rather than only on its name. The token is the
// session's Vault credential and it is the only secret the overlay admits, so
// a malformed one reaching a Terraform child is a credential in an
// environment nobody can account for.
func TestTheOverlayBoundsAShortLivedVaultToken(t *testing.T) {
	for name, token := range map[string]string{
		"empty":               "",
		"fifteen":             strings.Repeat("a", 15),
		"one past a thousand": strings.Repeat("a", 1001),
		"with a space":        strings.Repeat("a", 10) + " " + strings.Repeat("b", 10),
		"with a slash":        strings.Repeat("a", 10) + "/" + strings.Repeat("b", 10),
	} {
		t.Run(name, func(t *testing.T) {
			result, err := OverlayEnvironment(nil, []string{"VAULT_TOKEN=" + token})
			if err == nil {
				t.Fatalf("accepted Vault token %q", token)
			}
			if result != nil {
				t.Fatal("a refused Vault token still produced an environment")
			}
			if err.Error() != "invalid short-lived Vault token" {
				t.Fatalf("refusal %q is not the token refusal", err.Error())
			}
		})
	}
	for name, token := range map[string]string{
		"sixteen":    strings.Repeat("a", 16),
		"a thousand": strings.Repeat("a", 1000),
		"punctuated": "hvs." + strings.Repeat("A-b_9", 4),
	} {
		t.Run(name, func(t *testing.T) {
			result, err := OverlayEnvironment(nil, []string{"VAULT_TOKEN=" + token})
			if err != nil {
				t.Fatalf("refused Vault token of %d characters: %v", len(token), err)
			}
			if values := environmentMap(t, result); values["VAULT_TOKEN"] != token {
				t.Fatalf("VAULT_TOKEN=%q want %q", values["VAULT_TOKEN"], token)
			}
		})
	}
}

// TestAnInheritedVaultTokenIsDroppedWhileAnIssuedOneSurvives pins the
// asymmetry the two loops make between the same variable name. What this
// process inherited is never carried into a Terraform child; what the session
// minted for this operation is.
func TestAnInheritedVaultTokenIsDroppedWhileAnIssuedOneSurvives(t *testing.T) {
	issued := strings.Repeat("c", 24)
	result, err := OverlayEnvironment(
		[]string{"VAULT_TOKEN=" + strings.Repeat("s", 24), "VAULT_ADDR=https://vault.invalid", "PATH=/usr/bin"},
		[]string{"VAULT_TOKEN=" + issued},
	)
	if err != nil {
		t.Fatal(err)
	}
	values := environmentMap(t, result)
	if values["VAULT_TOKEN"] != issued {
		t.Fatalf("VAULT_TOKEN=%q want the issued token", values["VAULT_TOKEN"])
	}
	if _, exists := values["VAULT_ADDR"]; exists {
		t.Fatal("an inherited VAULT_ address survived the scrub")
	}
	if values["PATH"] != "/usr/bin" {
		t.Fatalf("PATH=%q want /usr/bin", values["PATH"])
	}
}

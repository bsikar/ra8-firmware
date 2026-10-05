// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"testing"
)

func TestPinnedOpenTofuToolchainMetadata(t *testing.T) {
	if pinnedOpenTofuVersion != "1.13.0" {
		t.Fatalf("pinned OpenTofu version = %q", pinnedOpenTofuVersion)
	}
	if digest, ok := pinnedOpenTofuBinarySHA256("linux", "amd64"); !ok || digest != "1d57b45894d3b646811dccd9837eb8f4cd22b37186c5a241aad0263660582770" {
		t.Fatalf("Linux AMD64 OpenTofu digest = %q, found=%t", digest, ok)
	}
	if digest, ok := pinnedOpenTofuBinarySHA256("darwin", "arm64"); !ok || digest != "632f95ef06da253a12bbaf4e1ae88718f1fb1882f407915435492b93461a6bc1" {
		t.Fatalf("Darwin ARM64 OpenTofu digest = %q, found=%t", digest, ok)
	}
	if digest, ok := pinnedOpenTofuBinarySHA256("windows", "amd64"); !ok || digest != "1ad6b85c0f3ce9cf85c073be3b9f5daf17f8f453ab336f216e80a1a9f35dee99" {
		t.Fatalf("Windows AMD64 OpenTofu digest = %q, found=%t", digest, ok)
	}
	if _, ok := pinnedOpenTofuBinarySHA256("linux", "arm64"); ok {
		t.Fatal("unreviewed OpenTofu platform was accepted")
	}
}

func TestOpenPinnedTerraformRuntimeRejectsDifferentVersionBeforeFilesystemAccess(t *testing.T) {
	config := TerraformConfig{Version: "1.10.9"}
	if _, err := OpenPinnedTerraformRuntime(context.Background(), config); err == nil {
		t.Fatal("a runtime outside the pinned OpenTofu version was accepted")
	}
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"errors"
	"runtime"
)

// pinnedOpenTofuVersion is the CLI release used by the provisioning runtime.
const pinnedOpenTofuVersion = "1.13.0"

// Binary SHA-256 values are for executable bytes from the official v1.13.0
// archives after their published SHA256SUMS entries have been verified.
func pinnedOpenTofuBinarySHA256(goos, goarch string) (string, bool) {
	switch goos + "/" + goarch {
	case "darwin/arm64":
		return "632f95ef06da253a12bbaf4e1ae88718f1fb1882f407915435492b93461a6bc1", true
	case "linux/amd64":
		return "1d57b45894d3b646811dccd9837eb8f4cd22b37186c5a241aad0263660582770", true
	case "windows/amd64":
		return "1ad6b85c0f3ce9cf85c073be3b9f5daf17f8f453ab336f216e80a1a9f35dee99", true
	default:
		return "", false
	}
}

// OpenPinnedTerraformRuntime applies the repository's OpenTofu version and
// platform binary pins before opening the Terraform-compatible runtime.
func OpenPinnedTerraformRuntime(ctx context.Context, config TerraformConfig) (*TerraformRuntime, error) {
	if config.Version != pinnedOpenTofuVersion {
		return nil, errors.New("OpenTofu version differs from the pinned provisioner release")
	}
	digest, ok := pinnedOpenTofuBinarySHA256(runtime.GOOS, runtime.GOARCH)
	if !ok || config.BinarySHA256 != digest {
		return nil, errors.New("OpenTofu executable digest differs from the pinned platform binary")
	}
	return OpenTerraformRuntime(ctx, config)
}

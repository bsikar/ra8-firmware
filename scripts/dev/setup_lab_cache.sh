#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Setup dedicated caching infrastructure on Proxmox VE (pve).
# Provides:
#   1. apt-cacher-ng (Port 3142): Line-rate local caching proxy for Debian & Ubuntu .deb packages.
#   2. Nginx Artifact Cache (Port 8080): Local HTTP cache serving pinned toolchains & binaries:
#      - ARM GNU Toolchain 13.3.rel1 (x86_64 & aarch64)
#      - Unicorn 2.1.4 release tarball
#      - Doxygen 1.16.1 binary
#      - Just 1.40.0 Windows binary
#
# Idempotent: safe to run repeatedly.

set -euo pipefail
umask 077

SSH_ALIAS="${SSH_ALIAS:-pve}"

info() { printf '\033[1;34m[INFO]\033[0m %s\n' "$*"; }
success() { printf '\033[1;32m[OK]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[WARN]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

remote_root() {
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n /bin/bash -s -- "$@"
}

info "Checking SSH connection to Proxmox host '$SSH_ALIAS'..."
if ! ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" /bin/true >/dev/null 2>&1; then
  die "Cannot connect to '$SSH_ALIAS' via SSH."
fi

info "Configuring apt-cacher-ng and Nginx artifact cache on '$SSH_ALIAS'..."
remote_root <<'REMOTE'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

# 1. Install apt-cacher-ng and nginx
apt-get update -qq
apt-get install -qq -y --no-install-recommends apt-cacher-ng nginx curl ca-certificates >/dev/null

# 2. Configure apt-cacher-ng
cat > /etc/apt-cacher-ng/acng.conf <<'EOF'
CacheDir: /var/cache/apt-cacher-ng
LogDir: /var/log/apt-cacher-ng
Port: 3142
BindAddress: 0.0.0.0
Remap-debrep: file:deb_mirror*.gz /debian ; file:backends_debian
Remap-uburep: file:ubuntu_mirrors /ubuntu ; file:backends_ubuntu
Remap-secdeb: security.debian.org ; security.debian.org
Remap-secubu: security.ubuntu.com ; security.ubuntu.com
ReportPage: acng-report.html
UnbufferLogs: 0
VerboseLog: 1
ExThreshold: 30
PassThroughPattern: .*
EOF

systemctl enable apt-cacher-ng >/dev/null 2>&1 || true
systemctl restart apt-cacher-ng

# 3. Configure Artifact Cache Directory and Nginx
CACHE_DIR="/var/lib/ra8-lab/cache"
mkdir -p "$CACHE_DIR"
chown -R www-data:www-data "$CACHE_DIR" 2>/dev/null || true
chmod 0755 "$CACHE_DIR"

cat > /etc/nginx/sites-available/ra8-lab-cache <<EOF
server {
    listen 8080 default_server;
    listen [::]:8080 default_server;
    server_name _;

    root $CACHE_DIR;
    autoindex on;
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;

    location / {
        try_files \$uri \$uri/ =404;
    }

    # Health check endpoint
    location /health {
        return 200 "ra8-lab-cache ok\n";
        add_header Content-Type text/plain;
    }
}
EOF

ln -sf /etc/nginx/sites-available/ra8-lab-cache /etc/nginx/sites-enabled/ra8-lab-cache
# Test and restart nginx
nginx -t >/dev/null 2>&1
systemctl enable nginx >/dev/null 2>&1 || true
systemctl restart nginx

# 4. Pre-seed pinned toolchains and artifacts
cd "$CACHE_DIR"

fetch_pinned() {
  local filename="$1"
  local url="$2"
  local sha="$3"
  if [[ -f "$filename" ]] && echo "$sha  $filename" | sha256sum -c --status 2>/dev/null; then
    echo "  [CACHED] $filename (checksum verified)"
    return 0
  fi
  echo "  [DOWNLOADING] $filename..."
  rm -f "$filename.tmp"
  if curl -fsSL --retry 5 --retry-delay 2 "$url" -o "$filename.tmp"; then
    if echo "$sha  $filename.tmp" | sha256sum -c --status 2>/dev/null; then
      mv -f "$filename.tmp" "$filename"
      chmod 0644 "$filename"
      echo "  [VERIFIED] $filename cached successfully."
    else
      echo "  [ERROR] Checksum mismatch for $filename" >&2
      rm -f "$filename.tmp"
      return 1
    fi
  else
    echo "  [WARN] Failed to download $url" >&2
    rm -f "$filename.tmp"
  fi
}

echo "Pre-seeding pinned build dependencies into $CACHE_DIR..."

# Unicorn 2.1.4
fetch_pinned "unicorn-2.1.4.tar.gz" \
  "https://codeload.github.com/unicorn-engine/unicorn/tar.gz/refs/tags/2.1.4" \
  "ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4" || true

# Arm GNU Toolchain 13.3.rel1 x86_64
fetch_pinned "arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz" \
  "https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz" \
  "95c011cee430e64dd6087c75c800f04b9c49832cc1000127a92a97f9c8d83af4" || true

# Arm GNU Toolchain 13.3.rel1 aarch64
fetch_pinned "arm-gnu-toolchain-13.3.rel1-aarch64-arm-none-eabi.tar.xz" \
  "https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-aarch64-arm-none-eabi.tar.xz" \
  "c8824bffd057afce2259f7618254e840715f33523a3d4e4294f471208f976764" || true

# Doxygen 1.16.1
fetch_pinned "doxygen-1.16.1.linux.bin.tar.gz" \
  "https://github.com/doxygen/doxygen/releases/download/Release_1_16_1/doxygen-1.16.1.linux.bin.tar.gz" \
  "a56f885d37e3aae08a99f638d17bbb381224c03a878d9e2dda4f9fa4baf1d8bd" || true

# Just 1.40.0 for Windows
if [[ ! -f "just-1.40.0-x86_64-pc-windows-msvc.zip" ]]; then
  echo "  [DOWNLOADING] just-1.40.0-x86_64-pc-windows-msvc.zip..."
  curl -fsSL --retry 5 --retry-delay 2 \
    "https://github.com/casey/just/releases/download/1.40.0/just-1.40.0-x86_64-pc-windows-msvc.zip" \
    -o "just-1.40.0-x86_64-pc-windows-msvc.zip" || true
fi

REMOTE

# Verify services remotely
info "Verifying cache services on '$SSH_ALIAS'..."
if ssh "$SSH_ALIAS" curl -fsSL http://127.0.0.1:3142/acng-report.html >/dev/null 2>&1; then
  success "apt-cacher-ng is online on port 3142."
else
  warn "apt-cacher-ng health check did not respond as expected."
fi

if ssh "$SSH_ALIAS" curl -fsSL http://127.0.0.1:8080/health >/dev/null 2>&1; then
  success "Nginx artifact cache is online on port 8080."
else
  warn "Nginx artifact cache health check did not respond as expected."
fi

success "Proxmox lab cache setup complete."

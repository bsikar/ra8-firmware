#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Build the reviewed Windows Server 2025 template (VM 9011) used by the
# disposable Proxmox Windows CI driver. The script talks to Proxmox only
# through the `pve` SSH host and writes only VM 9011 and the dedicated
# ra8-tf-lab datastore and pool.

set -euo pipefail
umask 077

SSH_ALIAS="pve"
TEMPLATE_ID=9011
TEMPLATE_NAME="ra8-lab-windows-template"
POOL_ID="ra8-tf-lab"
STORAGE_ID="ra8-tf-lab"
ISO_STORAGE="local"
WIN_ISO="WindowsServer2025_26100.1742.240906-0331.ge_release_svc_refresh_SERVER_EVAL_x64FRE_en-us.iso"
VIRTIO_ISO="virtio-win-0.1.271.iso"
UNATTEND_ISO_NAME="unattend-9011.iso"

run_dir=""
created=0

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required command is unavailable: $1"
}

remote_root() {
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n /bin/bash -s -- "$@"
}

cleanup_remote_on_error() {
  local rc=$?
  trap - EXIT
  if ((created)); then
    remote_root "$TEMPLATE_ID" "$TEMPLATE_NAME" <<'REMOTE' || true
set +e
template_id="$1"
expected_name="$2"
config="$(qm config "$template_id" 2>/dev/null)"
name="$(awk -F': ' '$1 == "name" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
if [[ "$name" == "$expected_name" && "$description" == *"RA8_LAB_TEMPLATE=windows-ci-v1"* ]]; then
  qm set "$template_id" --protection 0 >/dev/null 2>&1
  qm stop "$template_id" --timeout 10 >/dev/null 2>&1 || true
  qm destroy "$template_id" --purge 1 >/dev/null 2>&1
fi
REMOTE
  fi
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n rm -f -- "/var/lib/vz/template/iso/$UNATTEND_ISO_NAME" >/dev/null 2>&1 || true
  [[ -z "$run_dir" ]] || rm -rf -- "$run_dir"
  exit "$rc"
}

generate_autounattend() {
  cat <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend">
  <settings pass="windowsPE">
    <component name="Microsoft-Windows-International-Core-WinPE" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <SetupUILanguage>
        <UILanguage>en-US</UILanguage>
      </SetupUILanguage>
      <InputLocale>en-US</InputLocale>
      <SystemLocale>en-US</SystemLocale>
      <UILanguage>en-US</UILanguage>
      <UserLocale>en-US</UserLocale>
    </component>
    <component name="Microsoft-Windows-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <RunSynchronous>
        <RunSynchronousCommand wcm:action="add">
          <Order>1</Order>
          <Path>cmd.exe /c for %d in (C D E F G) do if exist %d:\drivers\vioscsi\vioscsi.inf drvload %d:\drivers\vioscsi\vioscsi.inf</Path>
          <Description>Load VirtIO SCSI Driver</Description>
        </RunSynchronousCommand>
        <RunSynchronousCommand wcm:action="add">
          <Order>2</Order>
          <Path>cmd.exe /c for %d in (C D E F G) do if exist %d:\drivers\NetKVM\netkvm.inf drvload %d:\drivers\NetKVM\netkvm.inf</Path>
          <Description>Load VirtIO Network Driver</Description>
        </RunSynchronousCommand>
        <RunSynchronousCommand wcm:action="add">
          <Order>3</Order>
          <Path>cmd.exe /c for %d in (C D E F G) do if exist %d:\diskpart.txt diskpart /s %d:\diskpart.txt</Path>
          <Description>Online Disk 0</Description>
        </RunSynchronousCommand>
      </RunSynchronous>
      <DiskConfiguration>
        <Disk wcm:action="add">
          <DiskID>0</DiskID>
          <WillWipeDisk>true</WillWipeDisk>
          <CreatePartitions>
            <CreatePartition wcm:action="add">
              <Order>1</Order>
              <Type>Primary</Type>
              <Size>500</Size>
            </CreatePartition>
            <CreatePartition wcm:action="add">
              <Order>2</Order>
              <Type>Primary</Type>
              <Extend>true</Extend>
            </CreatePartition>
          </CreatePartitions>
          <ModifyPartitions>
            <ModifyPartition wcm:action="add">
              <Order>1</Order>
              <PartitionID>1</PartitionID>
              <Format>NTFS</Format>
              <Label>System</Label>
              <Active>true</Active>
            </ModifyPartition>
            <ModifyPartition wcm:action="add">
              <Order>2</Order>
              <PartitionID>2</PartitionID>
              <Format>NTFS</Format>
              <Label>Windows</Label>
              <Letter>C</Letter>
            </ModifyPartition>
          </ModifyPartitions>
        </Disk>
      </DiskConfiguration>
      <ImageInstall>
        <OSImage>
          <InstallFrom>
            <MetaData wcm:action="add">
              <Key>/IMAGE/INDEX</Key>
              <Value>2</Value>
            </MetaData>
          </InstallFrom>
          <InstallTo>
            <DiskID>0</DiskID>
            <PartitionID>2</PartitionID>
          </InstallTo>
          <WillShowUI>OnError</WillShowUI>
        </OSImage>
      </ImageInstall>
      <UserData>
        <AcceptEula>true</AcceptEula>
        <FullName>RA8 Lab CI</FullName>
        <Organization>RA8 Firmware</Organization>
      </UserData>
    </component>
  </settings>
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <ComputerName>ra8-lab-win</ComputerName>
      <TimeZone>UTC</TimeZone>
    </component>
  </settings>
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <AutoLogon>
        <Password>
          <Value>Ra8LabPasswd123!</Value>
          <PlainText>true</PlainText>
        </Password>
        <Enabled>true</Enabled>
        <LogonCount>3</LogonCount>
        <Username>Administrator</Username>
      </AutoLogon>
      <UserAccounts>
        <AdministratorPassword>
          <Value>Ra8LabPasswd123!</Value>
          <PlainText>true</PlainText>
        </AdministratorPassword>
      </UserAccounts>
      <FirstLogonCommands>
        <SynchronousCommand wcm:action="add">
          <Order>1</Order>
          <Description>Install VirtIO Guest Tools</Description>
          <CommandLine>cmd.exe /c for %d in (C D E F G) do if exist %d:\virtio-win-gt-x64.msi start /wait msiexec /i %d:\virtio-win-gt-x64.msi /qn /norestart</CommandLine>
        </SynchronousCommand>
        <SynchronousCommand wcm:action="add">
          <Order>2</Order>
          <Description>Configure WinRM</Description>
          <CommandLine>powershell.exe -ExecutionPolicy Bypass -Command "winrm quickconfig -q; winrm set winrm/config/service '@{AllowUnencrypted=\"true\"}'; winrm set winrm/config/service/auth '@{Basic=\"true\"}'; netsh advfirewall firewall set rule group=\"Windows Remote Management\" new enable=yes"</CommandLine>
        </SynchronousCommand>
        <SynchronousCommand wcm:action="add">
          <Order>3</Order>
          <Description>Configure OpenSSH Server</Description>
          <CommandLine>powershell.exe -ExecutionPolicy Bypass -Command "if (Get-Service -Name sshd -ErrorAction SilentlyContinue) { Set-Service -Name sshd -StartupType Automatic; Start-Service sshd } else { New-Service -Name sshd -BinaryPathName 'C:\Windows\System32\OpenSSH\sshd.exe' -DisplayName 'OpenSSH SSH Server' -StartupType Automatic; Start-Service sshd }; netsh advfirewall firewall add rule name=\"OpenSSH\" dir=in action=allow protocol=TCP localport=22"</CommandLine>
        </SynchronousCommand>
        <SynchronousCommand wcm:action="add">
          <Order>4</Order>
          <Description>Copy Bootstrap Script</Description>
          <CommandLine>powershell.exe -ExecutionPolicy Bypass -Command "New-Item -ItemType Directory -Force -Path C:\setup; Get-PSDrive -PSProvider FileSystem | ForEach-Object { if (Test-Path ('{0}bootstrap.ps1' -f $_.Root)) { Copy-Item ('{0}bootstrap.ps1' -f $_.Root) -Destination C:\setup\bootstrap.ps1 -Force } }"</CommandLine>
        </SynchronousCommand>
        <SynchronousCommand wcm:action="add">
          <Order>5</Order>
          <Description>Register Bootstrap Task</Description>
          <CommandLine>powershell.exe -ExecutionPolicy Bypass -Command "Register-ScheduledTask -TaskName 'RA8LabBootstrap' -Action (New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-ExecutionPolicy Bypass -File C:\setup\bootstrap.ps1') -Trigger (New-ScheduledTaskTrigger -AtStartup) -User 'SYSTEM' -RunLevel Highest -Force"</CommandLine>
        </SynchronousCommand>
        <SynchronousCommand wcm:action="add">
          <Order>6</Order>
          <Description>Run bootstrap and shutdown</Description>
          <CommandLine>powershell.exe -ExecutionPolicy Bypass -Command "powershell.exe -ExecutionPolicy Bypass -File C:\setup\bootstrap.ps1; shutdown /s /t 10 /c 'Template preparation complete'"</CommandLine>
        </SynchronousCommand>
      </FirstLogonCommands>
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <HideLocalAccountScreen>true</HideLocalAccountScreen>
        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <NetworkLocation>Work</NetworkLocation>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
    </component>
  </settings>
</unattend>
EOF
}

main() {
  for tool in ssh scp; do
    require_cmd "$tool"
  done

  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" /bin/true >/dev/null
  run_dir="$(mktemp -d "${TMPDIR:-/tmp}/ra8-lab-win-template.XXXXXXXX")"
  chmod 0700 "$run_dir"
  trap cleanup_remote_on_error EXIT

  generate_autounattend > "$run_dir/Autounattend.xml"
  scp -q "$run_dir/Autounattend.xml" "$SSH_ALIAS:/tmp/Autounattend.xml"

  remote_root "$TEMPLATE_ID" "$TEMPLATE_NAME" "$POOL_ID" "$STORAGE_ID" "$ISO_STORAGE" "$WIN_ISO" "$VIRTIO_ISO" "$UNATTEND_ISO_NAME" <<'REMOTE'
set -euo pipefail
template_id="$1"
template_name="$2"
pool_id="$3"
storage_id="$4"
iso_storage="$5"
win_iso="$6"
virtio_iso="$7"
unattend_iso_name="$8"

[[ ! -e "/etc/pve/qemu-server/${template_id}.conf" ]] || {
  printf 'refusing to replace existing VM/template %s\n' "$template_id" >&2
  exit 1
}

# Generate unattend ISO with staged VirtIO drivers on Proxmox host
rm -rf /tmp/unattend_staging /tmp/virtio_mnt
mkdir -p /tmp/unattend_staging/drivers/vioscsi
mkdir -p /tmp/unattend_staging/drivers/NetKVM
mkdir -p /tmp/virtio_mnt
mount -o loop,ro "/var/lib/vz/template/iso/${virtio_iso}" /tmp/virtio_mnt
cp -r /tmp/virtio_mnt/vioscsi/2k25/amd64/* /tmp/unattend_staging/drivers/vioscsi/
cp -r /tmp/virtio_mnt/NetKVM/2k25/amd64/* /tmp/unattend_staging/drivers/NetKVM/
umount /tmp/virtio_mnt
rmdir /tmp/virtio_mnt
cp /tmp/Autounattend.xml /tmp/unattend_staging/Autounattend.xml
rm -f /tmp/Autounattend.xml
cat <<'DPEOF' > /tmp/unattend_staging/diskpart.txt
select disk 0
online disk
attributes disk clear readonly
DPEOF
cat <<'PSEOF' > /tmp/unattend_staging/bootstrap.ps1
$targetIp = '10.250.8.20'
$targetGw = '10.250.8.1'

Get-PSDrive -PSProvider FileSystem | ForEach-Object {
  $cand = Join-Path $_.Root 'openstack\content\0000'
  if (Test-Path $cand) {
    $txt = Get-Content $cand -Raw
    if ($txt -match 'address\s+([0-9.]+)') { $targetIp = $matches[1] }
    if ($txt -match 'gateway\s+([0-9.]+)') { $targetGw = $matches[1] }
  }
}

$adapter = Get-NetAdapter | Where-Object { $_.Status -eq 'Up' } | Select-Object -First 1
if ($adapter) {
  $curIp = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -eq $targetIp }
  if (-not $curIp) {
    Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -like '10.250.*' } | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
    New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress $targetIp -PrefixLength 24 -DefaultGateway $targetGw -ErrorAction SilentlyContinue
    Set-DnsClientServerAddress -InterfaceIndex $adapter.ifIndex -ServerAddresses '1.1.1.1' -ErrorAction SilentlyContinue
  }
}

$keys = @()
Get-PSDrive -PSProvider FileSystem | ForEach-Object {
  $driveRoot = $_.Root
  $candidates = @(
    (Join-Path $driveRoot 'openstack\latest\meta_data.json'),
    (Join-Path $driveRoot 'openstack\latest\user_data'),
    (Join-Path $driveRoot 'user-data'),
    (Join-Path $driveRoot 'user_data')
  )
  foreach ($path in $candidates) {
    if (Test-Path $path) {
      if ($path -like "*.json") {
        try {
          $json = Get-Content $path -Raw | ConvertFrom-Json
          if ($json.public_keys) {
            foreach ($prop in $json.public_keys.PSObject.Properties) {
              if ($prop.Value -match '^ssh-') { $keys += $prop.Value.Trim() }
            }
          }
        } catch {}
      } else {
        $content = Get-Content $path -Raw
        $matches = [regex]::Matches($content, 'ssh-(?:ed25519|rsa|ecdsa)[A-Za-z0-9+/=]+[^\r\n]*')
        foreach ($m in $matches) {
          $keys += $m.Value.Trim()
        }
      }
    }
  }
}

if ($keys.Count -gt 0) {
  $keys = $keys | Select-Object -Unique
  $adminDir = 'C:\Users\Administrator\.ssh'
  New-Item -ItemType Directory -Force -Path $adminDir | Out-Null
  $keys | Out-File -FilePath "$adminDir\authorized_keys" -Encoding ascii -Force

  $progDataSsh = 'C:\ProgramData\ssh'
  New-Item -ItemType Directory -Force -Path $progDataSsh | Out-Null
  $adminKeyPath = "$progDataSsh\administrators_authorized_keys"
  $keys | Out-File -FilePath $adminKeyPath -Encoding ascii -Force

  icacls $adminKeyPath /inheritance:r /grant 'Administrators:F' /grant 'SYSTEM:F' | Out-Null
  icacls "$adminDir\authorized_keys" /inheritance:r /grant 'Administrators:F' /grant 'SYSTEM:F' | Out-Null
}

New-ItemProperty -Path "HKLM:\SOFTWARE\OpenSSH" -Name DefaultShell -Value "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -PropertyType String -Force -ErrorAction SilentlyContinue | Out-Null

if (Get-Service -Name sshd -ErrorAction SilentlyContinue) {
  Set-Service -Name sshd -StartupType Automatic
  Restart-Service -Name sshd -Force -ErrorAction SilentlyContinue
}
PSEOF
genisoimage -o "/var/lib/vz/template/iso/${unattend_iso_name}" -J -r /tmp/unattend_staging
rm -rf /tmp/unattend_staging

# Create the Windows template VM
qm create "$template_id" \
  --name "$template_name" \
  --memory 8192 \
  --cores 4 \
  --cpu host \
  --ostype win10 \
  --scsihw virtio-scsi-single \
  --pool "$pool_id" \
  --description "Disposable Windows Server 2025 template; RA8_LAB_TEMPLATE=windows-ci-v1" \
  --agent enabled=1 \
  --onboot 0 \
  --bios seabios

# Allocate 64G virtual disk on dedicated datastore (SATA AHCI for native inbox driver boot)
qm set "$template_id" --sata0 "${storage_id}:64,discard=on,ssd=1" >/dev/null

# Attach optical media: Windows ISO, VirtIO ISO, and Unattend ISO
qm set "$template_id" \
  --ide0 "${iso_storage}:iso/${win_iso},media=cdrom" \
  --ide1 "${iso_storage}:iso/${virtio_iso},media=cdrom" \
  --ide3 "${iso_storage}:iso/${unattend_iso_name},media=cdrom" >/dev/null

# Set boot order to boot from Windows installation ISO first, then SATA disk
qm set "$template_id" --boot "order=ide0;sata0" >/dev/null

printf 'Created VM %s for unattended Windows Server 2025 installation.\n' "$template_id"
REMOTE

  created=1
  printf 'Starting VM %s to run automated Windows setup...\n' "$TEMPLATE_ID"
  remote_root "$TEMPLATE_ID" <<'REMOTE'
set -euo pipefail
template_id="$1"
qm start "$template_id"
# Send keypress via QMP to automatically bypass "Press any key to boot from CD or DVD"
for _ in {1..8}; do
  sleep 1
  echo '{"execute": "qmp_capabilities"}' | socat - "/var/run/qemu-server/${template_id}.qmp" 2>/dev/null || true
  echo '{"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "spc"}]}}' | socat - "/var/run/qemu-server/${template_id}.qmp" 2>/dev/null || true
done
REMOTE

  printf 'Waiting for Windows setup and automated shutdown of VM %s (this typically takes 10-15 minutes)...\n' "$TEMPLATE_ID"
  local finished=0
  for _ in {1..120}; do
    local status
    status="$(ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n qm status "$TEMPLATE_ID" 2>/dev/null | awk '{print $2}')"
    if [[ "$status" == "stopped" ]]; then
      finished=1
      break
    fi
    sleep 15
  done

  ((finished)) || die "Windows setup did not complete within the timeout period"

  printf 'Windows setup finished. Sealing VM %s as template...\n' "$TEMPLATE_ID"
  remote_root "$TEMPLATE_ID" "$UNATTEND_ISO_NAME" "$STORAGE_ID" <<'REMOTE'
set -euo pipefail
template_id="$1"
unattend_iso_name="$2"
storage_id="$3"

# Detach installer ISOs
qm set "$template_id" --delete ide0,ide1,ide3 >/dev/null

# Set boot order to SATA disk
qm set "$template_id" --boot order=sata0 >/dev/null

# Remove temporary unattend ISO from storage
rm -f "/var/lib/vz/template/iso/${unattend_iso_name}"

# Convert to template and protect
qm template "$template_id"
qm set "$template_id" --protection 1 >/dev/null
REMOTE

  trap - EXIT
  rm -rf -- "$run_dir"
  printf 'Successfully created protected Windows Server 2025 template %s (%s) on %s.\n' "$TEMPLATE_ID" "$TEMPLATE_NAME" "$STORAGE_ID"
}

main "$@"

#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Build the reviewed Server 2025 Windows lab template through the pve SSH host.
# The caller must select the template ID explicitly.

set -euo pipefail
umask 077

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
SSH_ALIAS=pve
TEMPLATE_ID=
UNATTEND_ISO_NAME=
TEMPLATE_NAME=ra8-lab-windows-template
POOL_ID=ra8-tf-lab
STORAGE_ID=ra8-tf-lab
ISO_STORAGE=local
WIN_ISO=WindowsServer2025_26100.1742.240906-0331.ge_release_svc_refresh_SERVER_EVAL_x64FRE_en-us.iso
VIRTIO_ISO=virtio-win-0.1.271.iso
CLOUDBASE_VERSION=1.1.8
CLOUDBASE_SHA256=0e7fa42e0cbc0ce7657f85730b0c6cc7afc4087a3639df0ff51a721a0be19bd5

cloudbase_msi_path() {
  local override="${1:-}"
  if [[ -n "$override" ]]; then
    printf '%s\n' "$override"
  else
    printf '%s\n' "${HOME}/ra8ci-work/toolchain/cloudbase-init-1.1.8/CloudbaseInitSetup_1_1_8_x64.msi"
  fi
}

CLOUDBASE_MSI="$(cloudbase_msi_path "${RA8_CLOUDBASE_MSI:-}")"

run_dir=
transferred=0
created=0
SCREENSHOT_REMOTE_PATH=
SERIAL_LOG_REMOTE_PATH=
SERIAL_LOG_REMOTE_PID_PATH=
SERIAL_LOG_LOCAL_PATH=
SERIAL_LOG_CAPTURED=0

say_error() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

remote_root() {
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n /bin/bash -s -- "$@"
}

prepare_serial_log() {
  local log_dir="$HOME/ra8ci-work/RA8FW-787"
  mkdir -p "$log_dir"
  chmod 700 "$log_dir"
  SERIAL_LOG_LOCAL_PATH="$(mktemp "$log_dir/serial-9012-XXXXXX")"
  chmod 600 "$SERIAL_LOG_LOCAL_PATH"
  SERIAL_LOG_REMOTE_PATH="/tmp/ra8fw787-9012-${BASHPID}.serial.log"
  SERIAL_LOG_REMOTE_PID_PATH="/tmp/ra8fw787-9012-${BASHPID}.serial.pid"
}

capture_serial_log() {
  [[ -n "$SERIAL_LOG_REMOTE_PATH" && -n "$SERIAL_LOG_REMOTE_PID_PATH" && -n "$SERIAL_LOG_LOCAL_PATH" ]] || return 0
  if remote_root "$TEMPLATE_ID" "$SERIAL_LOG_REMOTE_PATH" "$SERIAL_LOG_REMOTE_PID_PATH" >"$SERIAL_LOG_LOCAL_PATH" <<'REMOTE'
set -euo pipefail
vmid="$1"; log_path="$2"; pid_path="$3"
[[ "$vmid" == 9012 && "$log_path" =~ ^/tmp/ra8fw787-9012-[0-9]+\.serial\.log$ && "$pid_path" =~ ^/tmp/ra8fw787-9012-[0-9]+\.serial\.pid$ ]] || { echo 'refusing unexpected serial log target' >&2; exit 1; }
if [[ -f "$pid_path" ]]; then
  logger_pid="$(cat "$pid_path")"
  if [[ "$logger_pid" =~ ^[0-9]+$ ]]; then
    for _ in {1..20}; do
      kill -0 "$logger_pid" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "$logger_pid" 2>/dev/null; then
      kill -TERM "$logger_pid" 2>/dev/null || true
      for _ in {1..20}; do
        kill -0 "$logger_pid" 2>/dev/null || break
        sleep 0.1
      done
    fi
  fi
fi
[[ -f "$log_path" ]] || { echo 'build serial log is missing' >&2; exit 1; }
cat "$log_path"
rm -f -- "$pid_path" "$log_path"
REMOTE
  then
    chmod 600 "$SERIAL_LOG_LOCAL_PATH"
    SERIAL_LOG_CAPTURED=1
    printf 'Build serial log copied to %s\n' "$SERIAL_LOG_LOCAL_PATH" >&2
    return 0
  fi
  printf 'error: could not collect the build serial log; local path: %s\n' "$SERIAL_LOG_LOCAL_PATH" >&2
  return 1
}

validate_id() {
  [[ "$TEMPLATE_ID" == 9012 ]] || say_error 'template ID must be 9012'
  [[ "$TEMPLATE_NAME" == ra8-lab-windows-template && "$POOL_ID" == ra8-tf-lab && "$STORAGE_ID" == ra8-tf-lab ]] ||
    say_error 'Windows template identity is outside the reviewed allowlist'
}

parse_template_id() {
  [[ $# -eq 1 && "$1" != --selftest ]] ||
    say_error 'usage: prepare_proxmox_lab_windows_template.sh <template-id|--selftest>'
  TEMPLATE_ID="$1"
  UNATTEND_ISO_NAME="unattend-${TEMPLATE_ID}.iso"
  validate_id
}

cleanup_failure() {
  local rc=$?
  trap - EXIT
  if ((transferred)); then
    remote_root "$TEMPLATE_ID" "$TEMPLATE_NAME" "$created" "$SCREENSHOT_REMOTE_PATH" "$SERIAL_LOG_REMOTE_PATH" "$SERIAL_LOG_REMOTE_PID_PATH" <<'REMOTE' || true
set +e
vmid="$1"; expected_name="$2"; owned="$3"; screenshot_path="$4"; serial_log_path="$5"; serial_pid_path="$6"
config="$(qm config "$vmid" 2>/dev/null)"
name="$(awk -F': ' '$1 == "name" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
if [[ "$owned" == 1 && "$vmid" == 9012 && "$name" == "$expected_name" && "$description" == *'RA8_LAB_TEMPLATE=windows-ci-v1'* ]]; then
  qm set "$vmid" --protection 0 >/dev/null 2>&1
  qm stop "$vmid" --timeout 10 >/dev/null 2>&1 || true
  qm destroy "$vmid" --purge 1 >/dev/null 2>&1
fi
umount /tmp/ra8fw787-virtio-mnt 2>/dev/null || true
rm -rf -- /tmp/ra8fw787-unattend /tmp/ra8fw787-virtio-mnt
rm -f -- /tmp/ra8fw787-Autounattend.xml /tmp/ra8fw787-cloudbase.msi
if [[ "$owned" == 1 ]]; then
  rm -f -- "/var/lib/vz/template/iso/unattend-${vmid}.iso"
fi
if [[ "$screenshot_path" =~ ^/tmp/ra8fw787-9012-timeout-[0-9]+\.png$ ]]; then
  rm -f -- "$screenshot_path"
fi
if [[ "$serial_log_path" =~ ^/tmp/ra8fw787-9012-[0-9]+\.serial\.log$ && "$serial_pid_path" =~ ^/tmp/ra8fw787-9012-[0-9]+\.serial\.pid$ ]]; then
  if [[ -f "$serial_pid_path" ]]; then
    logger_pid="$(cat "$serial_pid_path")"
    [[ "$logger_pid" =~ ^[0-9]+$ ]] && kill -TERM "$logger_pid" 2>/dev/null || true
  fi
fi
REMOTE
  fi
  if ((transferred && SERIAL_LOG_CAPTURED == 0)); then
    capture_serial_log || true
  fi
  [[ -z "$run_dir" ]] || rm -rf -- "$run_dir"
  exit "$rc"
}

capture_timeout_agent_diagnostics() {
  local diag_dir="$HOME/ra8ci-work/RA8FW-787" diag_path
  mkdir -p "$diag_dir"
  chmod 700 "$diag_dir"
  diag_path="$(mktemp "$diag_dir/agent-${TEMPLATE_ID}-XXXXXX")"
  chmod 600 "$diag_path"
  remote_root "$TEMPLATE_ID" >"$diag_path" 2>&1 <<'REMOTE' || true
set -uo pipefail
vmid="$1"
[[ "$vmid" == 9012 ]] || { echo 'refusing agent diagnostics for an unexpected VMID'; exit 1; }
[[ "$(qm status "$vmid" | awk '{print $2}')" == running ]] || { echo 'agent diagnostics: build VM is not running'; exit 0; }
if ! timeout 20 qm agent "$vmid" ping >/dev/null 2>&1; then
  echo 'agent diagnostics: guest agent did not answer ping'
  exit 0
fi
echo 'agent diagnostics: guest agent answered ping'
timeout 90 qm guest exec "$vmid" --timeout 60 -- powershell.exe -NoProfile -NonInteractive -Command '"--- progress.log"; Get-Content C:\setup\progress.log -ErrorAction SilentlyContinue; "--- C:\setup"; Get-ChildItem C:\setup -ErrorAction SilentlyContinue | Format-Table Name,Length,LastWriteTime -AutoSize | Out-String -Width 200; "--- MsiInstaller events"; Get-WinEvent -FilterHashtable @{LogName="Application";ProviderName="MsiInstaller"} -MaxEvents 15 -ErrorAction SilentlyContinue | Format-List TimeCreated,Id,Message | Out-String -Width 300; "--- bootstrap.log tail"; Get-Content C:\setup\bootstrap.log -Tail 40 -ErrorAction SilentlyContinue; "--- cloudbase-init-msi.log tail"; Get-Content C:\setup\cloudbase-init-msi.log -Tail 60 -ErrorAction SilentlyContinue; "--- processes"; Get-Process msiexec,cloudbase*,python* -ErrorAction SilentlyContinue | Format-Table Id,ProcessName,StartTime -AutoSize | Out-String -Width 200; "--- services"; Get-Service cloudbase-init,QEMU-GA -ErrorAction SilentlyContinue | Format-Table Name,Status,StartType -AutoSize | Out-String -Width 200' ||
  echo 'agent diagnostics: guest exec failed or timed out'
REMOTE
  printf 'Timeout guest-agent diagnostics saved to %s\n' "$diag_path" >&2
}

capture_timeout_screenshot() {
  local screenshot_dir="$HOME/ra8ci-work/RA8FW-787" screenshot_tmp screenshot_path
  SCREENSHOT_REMOTE_PATH="/tmp/ra8fw787-${TEMPLATE_ID}-timeout-${BASHPID}.png"
  mkdir -p "$screenshot_dir"
  chmod 700 "$screenshot_dir"
  screenshot_tmp="$(mktemp "$screenshot_dir/timeout-${TEMPLATE_ID}-XXXXXX")"
  screenshot_path="${screenshot_tmp}.png"
  mv -- "$screenshot_tmp" "$screenshot_path"
  remote_root "$TEMPLATE_ID" "$SCREENSHOT_REMOTE_PATH" <<'REMOTE'
set -euo pipefail
vmid="$1"; path="$2"
[[ "$vmid" == 9012 && "$path" =~ ^/tmp/ra8fw787-9012-timeout-[0-9]+\.png$ ]] || { echo 'refusing unexpected timeout screenshot target' >&2; exit 1; }
[[ "$(qm status "$vmid" | awk '{print $2}')" == running ]] || { echo 'timeout screenshot requires the build VM to be running' >&2; exit 1; }
qmp_socket="/var/run/qemu-server/${vmid}.qmp"
[[ -S "$qmp_socket" ]] || { echo 'timeout screenshot QMP socket is missing' >&2; exit 1; }
response="$(printf '%s\n%s\n' '{"execute":"qmp_capabilities"}' "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$path\",\"format\":\"png\"}}" | socat -T 2 - "UNIX-CONNECT:$qmp_socket")"
return_count="$(grep -o '"return"' <<<"$response" | wc -l | tr -d ' ')"
[[ "$response" != *'"error"'* && "$return_count" -ge 2 ]] || { echo 'QMP screenshot command failed' >&2; exit 1; }
[[ -s "$path" ]] || { echo 'timeout screendump produced no file' >&2; exit 1; }
REMOTE
  if ! ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n cat "$SCREENSHOT_REMOTE_PATH" >"$screenshot_path"; then
    rm -f -- "$screenshot_path"
    say_error 'could not copy the timeout screendump from the Proxmox host'
  fi
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n rm -f -- "$SCREENSHOT_REMOTE_PATH"
  SCREENSHOT_REMOTE_PATH=
  printf 'Timeout screendump copied to %s\n' "$screenshot_path" >&2
}

generate_autounattend() {
  cat <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend">
  <settings pass="windowsPE">
    <component name="Microsoft-Windows-International-Core-WinPE" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <SetupUILanguage><UILanguage>en-US</UILanguage></SetupUILanguage>
      <InputLocale>en-US</InputLocale><SystemLocale>en-US</SystemLocale><UILanguage>en-US</UILanguage><UserLocale>en-US</UserLocale>
    </component>
    <component name="Microsoft-Windows-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <RunSynchronous>
        <RunSynchronousCommand wcm:action="add"><Order>1</Order><Path>cmd.exe /c for %d in (C D E F G) do if exist %d:\drivers\vioscsi\vioscsi.inf drvload %d:\drivers\vioscsi\vioscsi.inf</Path><Description>Load VirtIO SCSI</Description></RunSynchronousCommand>
        <RunSynchronousCommand wcm:action="add"><Order>2</Order><Path>cmd.exe /c for %d in (C D E F G) do if exist %d:\drivers\NetKVM\netkvm.inf drvload %d:\drivers\NetKVM\netkvm.inf</Path><Description>Load VirtIO network</Description></RunSynchronousCommand>
        <RunSynchronousCommand wcm:action="add"><Order>3</Order><Path>cmd.exe /c for %d in (C D E F G) do if exist %d:\diskpart.txt diskpart /s %d:\diskpart.txt</Path><Description>Prepare the Windows disk</Description></RunSynchronousCommand>
      </RunSynchronous>
      <DiskConfiguration>
        <Disk wcm:action="add"><DiskID>0</DiskID><WillWipeDisk>true</WillWipeDisk>
          <CreatePartitions><CreatePartition wcm:action="add"><Order>1</Order><Type>Primary</Type><Size>500</Size></CreatePartition><CreatePartition wcm:action="add"><Order>2</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition></CreatePartitions>
          <ModifyPartitions><ModifyPartition wcm:action="add"><Order>1</Order><PartitionID>1</PartitionID><Format>NTFS</Format><Label>System</Label><Active>true</Active></ModifyPartition><ModifyPartition wcm:action="add"><Order>2</Order><PartitionID>2</PartitionID><Format>NTFS</Format><Label>Windows</Label><Letter>C</Letter></ModifyPartition></ModifyPartitions>
        </Disk>
      </DiskConfiguration>
      <ImageInstall><OSImage><InstallFrom><MetaData wcm:action="add"><Key>/IMAGE/INDEX</Key><Value>1</Value></MetaData></InstallFrom><InstallTo><DiskID>0</DiskID><PartitionID>2</PartitionID></InstallTo><WillShowUI>OnError</WillShowUI></OSImage></ImageInstall>
      <UserData><AcceptEula>true</AcceptEula><FullName>RA8 Lab CI</FullName><Organization>RA8 Firmware</Organization></UserData>
    </component>
  </settings>
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><ComputerName>ra8-lab-win</ComputerName><TimeZone>UTC</TimeZone></component>
  </settings>
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <AutoLogon><Password><Value>__EPHEMERAL_BOOTSTRAP_PASSWORD__</Value><PlainText>true</PlainText></Password><Enabled>true</Enabled><LogonCount>1</LogonCount><Username>Administrator</Username></AutoLogon>
      <UserAccounts><AdministratorPassword><Value>__EPHEMERAL_BOOTSTRAP_PASSWORD__</Value><PlainText>true</PlainText></AdministratorPassword></UserAccounts>
      <FirstLogonCommands><SynchronousCommand wcm:action="add"><Order>1</Order><Description>Run the local offline bootstrap and Cloudbase sysprep</Description><CommandLine>powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "New-Item -ItemType Directory -Force -Path C:\setup; Get-PSDrive -PSProvider FileSystem | ForEach-Object { if (Test-Path ('{0}bootstrap.ps1' -f $_.Root)) { Copy-Item ('{0}bootstrap.ps1' -f $_.Root) -Destination C:\setup\bootstrap.ps1 -Force } }; &amp; C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\setup\bootstrap.ps1"</CommandLine></SynchronousCommand></FirstLogonCommands>
      <OOBE><HideEULAPage>true</HideEULAPage><HideLocalAccountScreen>true</HideLocalAccountScreen><HideOEMRegistrationScreen>true</HideOEMRegistrationScreen><HideOnlineAccountScreens>true</HideOnlineAccountScreens><HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE><NetworkLocation>Work</NetworkLocation><ProtectYourPC>3</ProtectYourPC></OOBE>
    </component>
  </settings>
</unattend>
XML
}

selftest() {
  if bash "$0" >/dev/null 2>&1; then
    say_error 'missing template ID selftest failed'
  fi
  local invalid_id_output
  if invalid_id_output="$(bash "$0" 9013 2>&1)"; then
    say_error 'invalid template ID selftest failed'
  fi
  [[ "$invalid_id_output" == *'template ID must be 9012'* ]] ||
    say_error 'invalid template ID was not rejected before effects'
  local legacy_id_output
  if legacy_id_output="$(bash "$0" 9011 2>&1)"; then
    say_error 'legacy template ID 9011 was accepted'
  fi
  [[ "$legacy_id_output" == *'template ID must be 9012'* ]] ||
    say_error 'legacy template ID refusal did not identify the only allowed target'
  parse_template_id 9012
  [[ "$TEMPLATE_ID" == 9012 ]] || say_error 'explicit template ID selftest failed'
  UNATTEND_ISO_NAME="unattend-${TEMPLATE_ID}.iso"
  [[ "$UNATTEND_ISO_NAME" == unattend-9012.iso ]] || say_error 'explicit unattend ISO selftest failed'
  [[ "$CLOUDBASE_VERSION" == 1.1.8 && "$CLOUDBASE_SHA256" =~ ^[0-9a-f]{64}$ ]] || say_error 'Cloudbase-Init pin selftest failed'
  [[ "$(cloudbase_msi_path)" == "${HOME}/ra8ci-work/toolchain/cloudbase-init-1.1.8/CloudbaseInitSetup_1_1_8_x64.msi" ]] ||
    say_error 'Cloudbase-Init MSI default path selftest failed'
  [[ "$(cloudbase_msi_path /tmp/cloudbase-selftest.msi)" == /tmp/cloudbase-selftest.msi ]] ||
    say_error 'Cloudbase-Init MSI override selftest failed'
  local xml
  xml="$(generate_autounattend)"
  [[ "$xml" == *'__EPHEMERAL_BOOTSTRAP_PASSWORD__'* ]] || say_error 'unattend credential placeholder selftest failed'
  [[ "$xml" != *'Ra8LabPasswd'* && "$xml" != *'Password123'* ]] || say_error 'static credential found in unattend selftest'
  grep -Fq "Start-Transcript -Path 'C:\setup\bootstrap.log'" "$0" || say_error 'bootstrap transcript selftest failed'
  grep -Fq 'function Write-BuildSerial' "$0" || say_error 'build serial logger selftest failed'
  grep -Fq 'COM1 write failed:' "$0" || say_error 'build serial failure reporting selftest failed'
  grep -Fq 'Windows bootstrap exception from the serial log:' "$0" || say_error 'exception text must be printed on the exception path'
  grep -Fq "Add-Content -LiteralPath 'C:\setup\progress.log'" "$0" || say_error 'progress file selftest failed'
  grep -Fq 'ProviderName="MsiInstaller"' "$0" || say_error 'MsiInstaller event diagnostics selftest failed'
  grep -Fq 'function Wait-BoundedMsi' "$0" || say_error 'bounded MSI wait selftest failed'
  grep -Fq "'LOGGINGSERIALPORTNAME=\"\"'" "$0" || say_error 'Cloudbase-Init must not claim COM1 during the bake'
  grep -Fq 'qm guest exec "$vmid" --timeout 60' "$0" || say_error 'timeout guest-agent diagnostics selftest failed'
  [[ "$(grep -c '^ *capture_timeout_agent_diagnostics$' "$0")" == 2 ]] || say_error 'both timeout paths must collect guest-agent diagnostics'
  grep -Fq '$process.HasExited' "$0" || say_error 'MSI wait must poll HasExited'
  if grep -Fq '$process.WaitFor''Exit(60000)' "$0"; then say_error 'MSI wait must not rely on a timed WaitForExit'; fi
  grep -Fq "Wait-BoundedMsi \$agent 'QEMU guest-agent' 'C:\\setup\\qemu-ga-msi.log' 5" "$0" || say_error 'guest-agent MSI cap must fit inside the host build limit'
  grep -Fq 'VirtIO serial device ready' "$0" || say_error 'VirtIO serial device gate selftest failed'
  if grep -Eq 'msiexec\.exe .* -Wait -PassThru' "$0"; then say_error 'MSI installs must use the bounded wait'; fi
  grep -Fq 'Bootstrap exception:' "$0" || say_error 'build serial exception logging selftest failed'
  grep -Fq 'VirtIO serial driver install started' "$0" || say_error 'VirtIO serial driver progress selftest failed'
  grep -Fq 'QEMU guest-agent MSI install started' "$0" || say_error 'guest-agent MSI serial progress selftest failed'
  grep -Fq 'Cloudbase-Init MSI install started' "$0" || say_error 'Cloudbase-Init MSI serial progress selftest failed'
  grep -Fq 'Cloudbase-Init configuration started' "$0" || say_error 'Cloudbase-Init config serial progress selftest failed'
  grep -Fq 'Cloudbase-Init sysprep launch' "$0" || say_error 'sysprep serial progress selftest failed'
  grep -Fq 'cloudbaseinit.plugins.common.sshpublickeys.SetUserSSHPublicKeysPlugin' "$0" || say_error 'Cloudbase SSH-key plugin selftest failed'
  grep -Fq 'username=Administrator' "$0" || say_error 'Cloudbase Administrator username selftest failed'
  grep -Fq 'AuthorizedKeysFile .ssh/authorized_keys' "$0" || say_error 'OpenSSH Administrator key path selftest failed'
  grep -Fq "icacls.exe \$adminSshDir /inheritance:r /grant:r" "$0" || say_error 'OpenSSH Administrator key ACL selftest failed'
  grep -Fq 'DefaultPassword, DefaultUserName, DefaultDomainName, AutoAdminLogon, AutoLogonCount, ForceAutoLogon' "$0" || say_error 'AutoLogon cleanup selftest failed'
  local forbidden_guard
  forbidden_guard='Password''Required'
  ! grep -Fq "$forbidden_guard" "$0" || say_error 'password-required guard remains in bootstrap'
  grep -Fq 'vioserial/2k25/amd64/vioser.inf' "$0" || say_error 'VirtIO serial driver payload selftest failed'
  if grep -Fq 'virtio-win-gt-x64''.msi' "$0"; then say_error 'the full VirtIO guest-tools MSI must not be installed'; fi
  local virtio_install_line agent_install_line autologon_clear_line sysprep_line
  virtio_install_line="$(grep -nF "\$virtioTools = Start-Process" "$0" | tail -n 1 | cut -d: -f1)"
  agent_install_line="$(grep -nF "\$agent = Start-Process" "$0" | tail -n 1 | cut -d: -f1)"
  autologon_clear_line="$(grep -nF "Remove-ItemProperty -Path \$winlogon -Name DefaultPassword" "$0" | tail -n 1 | cut -d: -f1)"
  sysprep_line="$(grep -nF "& \$sysprep /generalize" "$0" | tail -n 1 | cut -d: -f1)"
  [[ -n "$virtio_install_line" && -n "$agent_install_line" && "$virtio_install_line" -lt "$agent_install_line" ]] ||
    say_error 'VirtIO tools must install before QEMU guest agent'
  [[ -n "$autologon_clear_line" && -n "$sysprep_line" && "$autologon_clear_line" -lt "$sysprep_line" ]] ||
    say_error 'AutoLogon values must be cleared before sysprep'
  grep -Fq 'keys     = [var.ssh_public_key]' "$SCRIPT_DIR/../../infra/terraform/environments/lab-guest/main.tf" ||
    say_error 'lab-guest configdrive SSH key wiring selftest failed'
  grep -Fq 'start_guest_ssh_proxy guest_windows' "$SCRIPT_DIR/../../infra/terraform/lab-guest.sh" ||
    say_error 'Windows SSH loopback proxy selftest failed'
  grep -Fq 'screendump\",\"arguments' "$0" ||
    say_error 'timeout screendump selftest failed'
  grep -Fq '"format":"png"' "$0" || say_error 'timeout screendump PNG format selftest failed'
  grep -Fq -- '--serial0 socket' "$0" || say_error 'build serial socket selftest failed'
  grep -Fq 'capture_serial_log' "$0" || say_error 'build serial capture selftest failed'
  grep -Fq "socat -u \"UNIX-CONNECT:\$socket_path\"" "$0" || say_error 'build serial socket capture command selftest failed'
  local serial_remove_line template_line
  serial_remove_line="$(grep -nF "qm set \"\$vmid\" --delete ide0,ide1,ide3,serial0" "$0" | tail -n 1 | cut -d: -f1)"
  template_line="$(grep -nF "qm template \"\$vmid\"" "$0" | tail -n 1 | cut -d: -f1)"
  [[ -n "$serial_remove_line" && -n "$template_line" && "$serial_remove_line" -lt "$template_line" ]] ||
    say_error 'build serial device must be removed before template conversion'
  printf '%s\n' 'prepare_proxmox_lab_windows_template.sh --selftest: PASS'
}

main() {
  if [[ "${1:-}" == --selftest ]]; then
    [[ $# -eq 1 ]] || say_error 'usage: prepare_proxmox_lab_windows_template.sh --selftest'
    selftest
    return
  fi
  parse_template_id "$@"
  for tool in ssh scp openssl shasum; do command -v "$tool" >/dev/null 2>&1 || say_error "required command is unavailable: $tool"; done
  [[ -f "$CLOUDBASE_MSI" && ! -L "$CLOUDBASE_MSI" ]] || say_error 'pinned Cloudbase-Init MSI is missing'
  [[ "$(shasum -a 256 "$CLOUDBASE_MSI" | awk '{print $1}')" == "$CLOUDBASE_SHA256" ]] || say_error 'Cloudbase-Init MSI SHA-256 does not match the pin'
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" /bin/true >/dev/null
  run_dir="$(mktemp -d "${TMPDIR:-/tmp}/ra8fw787-template.XXXXXXXX")"
  chmod 0700 "$run_dir"
  prepare_serial_log
  trap cleanup_failure EXIT
  local ephemeral_password
  ephemeral_password="$(openssl rand -hex 32)"
  generate_autounattend | sed "s/__EPHEMERAL_BOOTSTRAP_PASSWORD__/${ephemeral_password}/g" > "$run_dir/Autounattend.xml"
  unset ephemeral_password
  chmod 0600 "$run_dir/Autounattend.xml"
  remote_root "$TEMPLATE_ID" <<'REMOTE'
set -euo pipefail
vmid="$1"
[[ ! -e "/etc/pve/qemu-server/${vmid}.conf" ]] || { echo 'refusing to replace an existing VM/template' >&2; exit 1; }
for path in /tmp/ra8fw787-unattend /tmp/ra8fw787-virtio-mnt /tmp/ra8fw787-Autounattend.xml /tmp/ra8fw787-cloudbase.msi "/var/lib/vz/template/iso/unattend-${vmid}.iso"; do
  [[ ! -e "$path" ]] || { echo 'refusing to overwrite pre-existing temporary build data' >&2; exit 1; }
done
REMOTE
  transferred=1
  scp -q "$run_dir/Autounattend.xml" "$SSH_ALIAS:/tmp/ra8fw787-Autounattend.xml"
  scp -q "$CLOUDBASE_MSI" "$SSH_ALIAS:/tmp/ra8fw787-cloudbase.msi"
  remote_root "$TEMPLATE_ID" "$TEMPLATE_NAME" "$POOL_ID" "$STORAGE_ID" "$ISO_STORAGE" "$WIN_ISO" "$VIRTIO_ISO" "$CLOUDBASE_SHA256" "$SERIAL_LOG_REMOTE_PATH" "$SERIAL_LOG_REMOTE_PID_PATH" <<'REMOTE'
set -euo pipefail
vmid="$1"; expected_name="$2"; pool="$3"; storage="$4"; iso_storage="$5"; windows_iso="$6"; virtio_iso="$7"; expected_sha="$8"; serial_log_path="$9"; serial_pid_path="${10}"
[[ "$vmid" == 9012 && "$expected_name" == ra8-lab-windows-template && "$pool" == ra8-tf-lab && "$storage" == ra8-tf-lab ]] || { echo 'refusing unexpected template identity' >&2; exit 1; }
[[ "$serial_log_path" =~ ^/tmp/ra8fw787-9012-[0-9]+\.serial\.log$ && "$serial_pid_path" =~ ^/tmp/ra8fw787-9012-[0-9]+\.serial\.pid$ ]] || { echo 'refusing unexpected build serial log path' >&2; exit 1; }
config_path="/etc/pve/qemu-server/${vmid}.conf"
[[ ! -e "$config_path" ]] || { echo 'refusing to replace an existing VM/template' >&2; exit 1; }
win_path="/var/lib/vz/template/iso/${windows_iso}"
virtio_path="/var/lib/vz/template/iso/${virtio_iso}"
[[ -f "$win_path" && -f "$virtio_path" && -f /tmp/ra8fw787-Autounattend.xml && -f /tmp/ra8fw787-cloudbase.msi ]] || { echo 'required local ISO or transfer is missing' >&2; exit 1; }
[[ "$(sha256sum /tmp/ra8fw787-cloudbase.msi | awk '{print $1}')" == "$expected_sha" ]] || { echo 'Cloudbase-Init MSI digest mismatch on controller' >&2; exit 1; }
for path in /tmp/ra8fw787-unattend /tmp/ra8fw787-virtio-mnt "/var/lib/vz/template/iso/unattend-${vmid}.iso" "$serial_log_path" "$serial_pid_path"; do
  [[ ! -e "$path" ]] || { echo 'refusing to overwrite pre-existing temporary build data' >&2; exit 1; }
done
: > "$serial_log_path"
chmod 600 "$serial_log_path"
remote_created=0
cleanup_build() {
  rc=$?
  trap - EXIT
  umount /tmp/ra8fw787-virtio-mnt 2>/dev/null || true
  rm -rf -- /tmp/ra8fw787-unattend /tmp/ra8fw787-virtio-mnt
  rm -f -- /tmp/ra8fw787-Autounattend.xml /tmp/ra8fw787-cloudbase.msi
  rm -f -- "/var/lib/vz/template/iso/unattend-${vmid}.iso"
  if [[ -f "$serial_pid_path" ]]; then
    logger_pid="$(cat "$serial_pid_path")"
    [[ "$logger_pid" =~ ^[0-9]+$ ]] && kill -TERM "$logger_pid" 2>/dev/null || true
  fi
  rm -f -- "$serial_log_path" "$serial_pid_path"
  if ((remote_created)); then
    cfg="$(qm config "$vmid" 2>/dev/null)"
    vm_name="$(awk -F': ' '$1 == "name" {print $2; exit}' <<<"$cfg")"
    vm_description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$cfg")"
    if [[ "$vm_name" == "$expected_name" && "$vm_description" == *'RA8_LAB_TEMPLATE=windows-ci-v1'* ]]; then
      qm set "$vmid" --protection 0 >/dev/null 2>&1
      qm stop "$vmid" --timeout 10 >/dev/null 2>&1 || true
      qm destroy "$vmid" --purge 1 >/dev/null 2>&1
      rm -f -- "/var/lib/vz/template/iso/unattend-${vmid}.iso"
    fi
  fi
  exit "$rc"
}
trap cleanup_build EXIT
mkdir -p /tmp/ra8fw787-unattend/drivers/vioscsi /tmp/ra8fw787-unattend/drivers/NetKVM /tmp/ra8fw787-unattend/payload/vioserial /tmp/ra8fw787-virtio-mnt
mount -o loop,ro "$virtio_path" /tmp/ra8fw787-virtio-mnt
cp -r /tmp/ra8fw787-virtio-mnt/vioscsi/2k25/amd64/. /tmp/ra8fw787-unattend/drivers/vioscsi/
cp -r /tmp/ra8fw787-virtio-mnt/NetKVM/2k25/amd64/. /tmp/ra8fw787-unattend/drivers/NetKVM/
[[ -f /tmp/ra8fw787-virtio-mnt/guest-agent/qemu-ga-x86_64.msi ]] || { umount /tmp/ra8fw787-virtio-mnt; echo 'VirtIO guest-agent MSI missing from local ISO' >&2; exit 1; }
[[ -f /tmp/ra8fw787-virtio-mnt/vioserial/2k25/amd64/vioser.inf ]] || { echo 'VirtIO serial driver missing from the local ISO (vioserial/2k25/amd64); vioserial holds:' >&2; ls -1 /tmp/ra8fw787-virtio-mnt/vioserial >&2 || true; umount /tmp/ra8fw787-virtio-mnt; exit 1; }
cp -r /tmp/ra8fw787-virtio-mnt/vioserial/2k25/amd64/. /tmp/ra8fw787-unattend/payload/vioserial/
cp /tmp/ra8fw787-virtio-mnt/guest-agent/qemu-ga-x86_64.msi /tmp/ra8fw787-unattend/payload/qemu-ga-x86_64.msi
umount /tmp/ra8fw787-virtio-mnt
rmdir /tmp/ra8fw787-virtio-mnt
cp /tmp/ra8fw787-cloudbase.msi /tmp/ra8fw787-unattend/payload/CloudbaseInitSetup_1_1_8_x64.msi
cp /tmp/ra8fw787-Autounattend.xml /tmp/ra8fw787-unattend/Autounattend.xml
cat > /tmp/ra8fw787-unattend/diskpart.txt <<'DISKPART'
select disk 0
online disk
attributes disk clear readonly
DISKPART
cat > /tmp/ra8fw787-unattend/bootstrap.ps1 <<'POWERSHELL'
$ErrorActionPreference = 'Stop'
$transcriptStarted = $false
function Write-BuildSerial([string]$Message) {
  $line = "[RA8FW-787] $Message (t=$(Get-Date -Format HH:mm:ss))"
  # The console copy lands in the transcript and the timeout screendump, so
  # progress stays visible even when COM1 is unavailable.
  Write-Host $line
  # The transcript buffers and the screen freezes, so an unbuffered file is
  # the record the host reads back through the guest agent.
  try { Add-Content -LiteralPath 'C:\setup\progress.log' -Value $line -Encoding ASCII } catch { }
  $port = $null
  try {
    $port = [System.IO.Ports.SerialPort]::new('COM1', 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $port.WriteTimeout = 5000
    $port.Open()
    $port.WriteLine($line)
  } catch {
    Write-Host "[RA8FW-787] COM1 write failed: $($_.Exception.Message)"
    return
  } finally {
    if ($port) {
      if ($port.IsOpen) { $port.Close() }
      $port.Dispose()
    }
  }
}
function Write-MsiLogTail([string]$logPath) {
  if (Test-Path -LiteralPath $logPath) {
    foreach ($line in (Get-Content -LiteralPath $logPath -Tail 30)) { Write-BuildSerial "msi-log: $line" }
  } else {
    Write-BuildSerial "msi-log: $logPath was not created"
  }
}
function Wait-BoundedMsi([System.Diagnostics.Process]$process, [string]$label, [string]$logPath, [int]$minutes) {
  # Poll instead of WaitForExit(timeout): three bakes stalled inside an
  # install with no heartbeat at all, so the wait never returned. A poll
  # tells a stuck wait apart from a frozen guest.
  $finished = $false
  $clock = [System.Diagnostics.Stopwatch]::StartNew()
  $nextBeat = 60
  while ($clock.Elapsed.TotalMinutes -lt $minutes) {
    $process.Refresh()
    if ($process.HasExited) { $finished = $true; break }
    if ($clock.Elapsed.TotalSeconds -ge $nextBeat) {
      $running = @(Get-Process msiexec -ErrorAction SilentlyContinue).Count
      Write-BuildSerial "$label install still running after $([int]$clock.Elapsed.TotalSeconds)s; msiexec processes=$running"
      $nextBeat += 60
    }
    Start-Sleep -Seconds 5
  }
  if ($finished) { $process.WaitForExit() }
  if (-not $finished) {
    Write-BuildSerial "$label MSI install exceeded $minutes minutes; stopping msiexec"
    Write-MsiLogTail $logPath
    Get-Process msiexec -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    throw "$label MSI install timed out."
  }
  if ($process.ExitCode -notin @(0, 3010)) {
    Write-MsiLogTail $logPath
    throw "$label MSI install failed with exit $($process.ExitCode)."
  }
}
try {
New-Item -ItemType Directory -Force -Path 'C:\setup' | Out-Null
Start-Transcript -Path 'C:\setup\bootstrap.log' -Append
$transcriptStarted = $true
Write-BuildSerial 'Bootstrap started'
$payload = $null
foreach ($drive in Get-PSDrive -PSProvider FileSystem) {
  $candidate = Join-Path $drive.Root 'payload\CloudbaseInitSetup_1_1_8_x64.msi'
  if (Test-Path -LiteralPath $candidate) { $payload = Join-Path $drive.Root 'payload'; break }
}
if (-not $payload) { throw 'Local bootstrap payload is unavailable.' }
$agentMsi = Join-Path $payload 'qemu-ga-x86_64.msi'
$vioserialInf = Join-Path $payload 'vioserial\vioser.inf'
$cloudbaseMsi = Join-Path $payload 'CloudbaseInitSetup_1_1_8_x64.msi'
foreach ($installer in @($vioserialInf, $agentMsi, $cloudbaseMsi)) { if (-not (Test-Path -LiteralPath $installer)) { throw 'Required local installer is missing.' } }
# Only the VirtIO serial driver is needed: the guest agent talks over it.
# The full guest-tools MSI also installs display, balloon and other drivers,
# and its install stalled bootstrap in earlier bakes.
Write-BuildSerial 'VirtIO serial driver install started'
$virtioTools = Start-Process pnputil.exe -ArgumentList @('/add-driver', $vioserialInf, '/install') -PassThru -WindowStyle Hidden
Wait-BoundedMsi $virtioTools 'VirtIO serial driver' 'C:\setup\no-msi-log-for-pnputil' 5
Write-BuildSerial "VirtIO serial driver install complete; exit=$($virtioTools.ExitCode)"
# The guest agent's service opens the VirtIO serial port when it starts, so
# bind the freshly installed driver and prove the port exists before the
# agent MSI tries to start that service.
& pnputil.exe /scan-devices | Out-Null
$serialDevice = $null
for ($attempt = 0; $attempt -lt 24 -and -not $serialDevice; $attempt++) {
  $serialDevice = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -like '*VirtIO Serial*' -and $_.Status -eq 'OK' } | Select-Object -First 1
  if (-not $serialDevice) { Start-Sleep -Seconds 5 }
}
if (-not $serialDevice) {
  foreach ($device in (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like '*VirtIO*' -or $_.Status -ne 'OK' })) {
    Write-BuildSerial "pnp: $($device.Status) $($device.Class) $($device.FriendlyName) $($device.InstanceId)"
  }
  throw 'VirtIO serial device did not bind after driver install.'
}
Write-BuildSerial "VirtIO serial device ready: $($serialDevice.FriendlyName)"
Write-BuildSerial 'QEMU guest-agent MSI install started'
$agent = Start-Process msiexec.exe -ArgumentList @('/i', $agentMsi, '/qn', '/norestart', '/l*v', 'C:\setup\qemu-ga-msi.log') -PassThru
Wait-BoundedMsi $agent 'QEMU guest-agent' 'C:\setup\qemu-ga-msi.log' 5
Write-BuildSerial "QEMU guest-agent MSI install complete; exit=$($agent.ExitCode)"
Write-BuildSerial 'Cloudbase-Init MSI install started'
# The MSI defaults Cloudbase-Init's own logging to COM1, the port these
# progress lines use. Serial output stopped at this step in the last bake, so
# the installer gets no logging port.
$cloudbase = Start-Process msiexec.exe -ArgumentList @('/i', $cloudbaseMsi, '/qn', '/norestart', 'RUN_SERVICE_AS_LOCAL_SYSTEM=1', 'LOGGINGSERIALPORTNAME=""', '/l*v', 'C:\setup\cloudbase-init-msi.log') -PassThru
Wait-BoundedMsi $cloudbase 'Cloudbase-Init' 'C:\setup\cloudbase-init-msi.log' 10
Write-BuildSerial "Cloudbase-Init MSI install complete; exit=$($cloudbase.ExitCode)"
$root = Join-Path $env:ProgramFiles 'Cloudbase Solutions\Cloudbase-Init'
$conf = Join-Path $root 'conf'
Write-BuildSerial 'Cloudbase-Init configuration started'
$mainConfig = @'
[DEFAULT]
username=Administrator
groups=Administrators
inject_user_password=false
first_logon_behaviour=no
allow_reboot=false
config_drive_cdrom=true
config_drive_raw_hhd=true
metadata_services=cloudbaseinit.metadata.services.configdrive.ConfigDriveService
plugins=cloudbaseinit.plugins.common.sethostname.SetHostNamePlugin,cloudbaseinit.plugins.windows.networkconfig.NetworkConfigPlugin,cloudbaseinit.plugins.common.sshpublickeys.SetUserSSHPublicKeysPlugin
'@
$unattendConfig = @'
[DEFAULT]
username=Administrator
allow_reboot=false
config_drive_cdrom=true
config_drive_raw_hhd=true
metadata_services=cloudbaseinit.metadata.services.configdrive.ConfigDriveService
plugins=cloudbaseinit.plugins.common.sethostname.SetHostNamePlugin,cloudbaseinit.plugins.common.sshpublickeys.SetUserSSHPublicKeysPlugin
'@
Set-Content -LiteralPath (Join-Path $conf 'cloudbase-init.conf') -Value $mainConfig -Encoding ASCII
Set-Content -LiteralPath (Join-Path $conf 'cloudbase-init-unattend.conf') -Value $unattendConfig -Encoding ASCII
Write-BuildSerial 'Cloudbase-Init configuration complete'
$unattend = Join-Path $conf 'Unattend.xml'
if (-not (Test-Path -LiteralPath $unattend)) { throw 'Cloudbase-Init Unattend.xml is missing.' }
$svc = Get-Service -Name cloudbase-init -ErrorAction Stop
Set-Service -Name $svc.Name -StartupType Automatic
Stop-Service -Name $svc.Name -Force -ErrorAction SilentlyContinue
$null = & net.exe user Administrator ''
if ($LASTEXITCODE -ne 0) { throw 'Could not clear the setup-only password.' }
$winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
Remove-ItemProperty -Path $winlogon -Name DefaultPassword, DefaultUserName, DefaultDomainName, AutoAdminLogon, AutoLogonCount, ForceAutoLogon -ErrorAction SilentlyContinue
$adminSshDir = Join-Path $env:USERPROFILE '.ssh'
New-Item -ItemType Directory -Force -Path $adminSshDir | Out-Null
& icacls.exe $adminSshDir /inheritance:r /grant:r 'Administrator:(OI)(CI)F' 'SYSTEM:(OI)(CI)F' 'Administrators:(OI)(CI)F' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not secure the Administrator SSH key directory.' }
$sshdConfig = Join-Path $env:ProgramData 'ssh\sshd_config'
if (-not (Test-Path -LiteralPath $sshdConfig)) { throw 'OpenSSH Server config is missing; cannot configure Administrator SSH keys.' }
$sshdText = Get-Content -LiteralPath $sshdConfig -Raw
$adminMatch = '(?im)(^\s*Match\s+Group\s+administrators\s*\r?\n)(\s*AuthorizedKeysFile\s+[^\r\n]+)'
if ($sshdText -notmatch $adminMatch) { throw 'OpenSSH Administrator authorized-keys rule is missing.' }
$sshdText = [regex]::Replace($sshdText, $adminMatch, '$1    AuthorizedKeysFile .ssh/authorized_keys', 1)
Set-Content -LiteralPath $sshdConfig -Value $sshdText -Encoding ASCII
$sshd = Join-Path $env:SystemRoot 'System32\OpenSSH\sshd.exe'
if (-not (Test-Path -LiteralPath $sshd)) { throw 'OpenSSH Server executable is missing.' }
& $sshd -t -f $sshdConfig
if ($LASTEXITCODE -ne 0) { throw 'OpenSSH Server configuration validation failed.' }
foreach ($dir in @('C:\Windows\Panther\Unattend', 'C:\Windows\Panther\UnattendGC')) { if (Test-Path $dir) { Remove-Item $dir -Recurse -Force } }
foreach ($file in @('C:\Windows\Panther\Unattend.xml', 'C:\Windows\Panther\unattend.xml', 'C:\Windows\System32\Sysprep\unattend.xml')) { if (Test-Path $file) { Remove-Item $file -Force } }
$sysprep = Join-Path $env:SystemRoot 'System32\Sysprep\sysprep.exe'
Write-BuildSerial 'Cloudbase-Init sysprep launch'
& $sysprep /generalize /oobe /shutdown "/unattend:$unattend"
if ($LASTEXITCODE -ne 0) { throw 'Cloudbase-Init sysprep failed.' }
} catch {
  $message = $_.Exception.Message
  Write-BuildSerial "Bootstrap exception: $message"
  Write-Host "Bootstrap error: $message" -ForegroundColor Red
  if ($transcriptStarted) { Stop-Transcript; $transcriptStarted = $false }
  Write-Host 'Bootstrap failed; review C:\setup\bootstrap.log. Press Enter to keep this console open.'
  Read-Host | Out-Null
  exit 1
} finally {
  if ($transcriptStarted) { Stop-Transcript }
}
POWERSHELL
chmod 0600 /tmp/ra8fw787-Autounattend.xml /tmp/ra8fw787-cloudbase.msi
genisoimage -quiet -o "/var/lib/vz/template/iso/unattend-${vmid}.iso" -J -r /tmp/ra8fw787-unattend
rm -rf -- /tmp/ra8fw787-unattend
rm -f -- /tmp/ra8fw787-Autounattend.xml /tmp/ra8fw787-cloudbase.msi
qm create "$vmid" --name "$expected_name" --memory 8192 --cores 4 --cpu host --ostype win10 --scsihw virtio-scsi-single --serial0 socket --pool "$pool" --description 'Generalized Server 2025 lab template; RA8_LAB_TEMPLATE=windows-ci-v1' --agent enabled=1 --onboot 0 --bios seabios
remote_created=1
qm set "$vmid" --sata0 "${storage}:64,discard=on,ssd=1" >/dev/null
qm set "$vmid" --ide0 "${iso_storage}:iso/${windows_iso},media=cdrom" --ide1 "${iso_storage}:iso/${virtio_iso},media=cdrom" --ide3 "${iso_storage}:iso/unattend-${vmid}.iso,media=cdrom" >/dev/null
qm set "$vmid" --boot 'order=ide0;sata0' >/dev/null
printf 'Created approved template VM %s from the local Server and VirtIO ISOs.\n' "$vmid"
trap - EXIT
remote_created=0
REMOTE
  created=1
  printf 'Starting VM %s for offline Windows installation and generalization.\n' "$TEMPLATE_ID"
  remote_root "$TEMPLATE_ID" "$SERIAL_LOG_REMOTE_PATH" "$SERIAL_LOG_REMOTE_PID_PATH" <<'REMOTE'
set -euo pipefail
vmid="$1"; serial_log_path="$2"; serial_pid_path="$3"
  [[ "$vmid" == 9012 ]] || { echo 'refusing to start an unallowlisted template VMID' >&2; exit 1; }
[[ "$serial_log_path" =~ ^/tmp/ra8fw787-9012-[0-9]+\.serial\.log$ && "$serial_pid_path" =~ ^/tmp/ra8fw787-9012-[0-9]+\.serial\.pid$ ]] || { echo 'refusing unexpected build serial log path' >&2; exit 1; }
config="$(qm config "$vmid")"
[[ "$config" == *'name: ra8-lab-windows-template'* && "$config" == *'RA8_LAB_TEMPLATE=windows-ci-v1'* ]] || { echo 'refusing start: VM identity mismatch' >&2; exit 1; }
qm start "$vmid"
socket_path="/var/run/qemu-server/${vmid}.serial0"
for _ in {1..50}; do
  [[ -S "$socket_path" ]] && break
  sleep 0.1
done
[[ -S "$socket_path" ]] || { echo 'build serial socket did not appear' >&2; exit 1; }
nohup socat -u "UNIX-CONNECT:$socket_path" "OPEN:$serial_log_path,creat,append" >/dev/null 2>&1 </dev/null &
logger_pid=$!
printf '%s\n' "$logger_pid" > "$serial_pid_path"
sleep 0.2
kill -0 "$logger_pid" 2>/dev/null || { echo 'build serial capture process exited' >&2; exit 1; }
{
  printf '%s\n' '{"execute":"qmp_capabilities"}'
  sleep 1
  for _ in {1..8}; do
    printf '%s\n' '{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"spc"}]}}'
    sleep 1
  done
} | socat - "UNIX-CONNECT:/var/run/qemu-server/${vmid}.qmp" >/dev/null
REMOTE
  local finished=0 started=0 status
  printf 'Waiting for Windows setup and sysprep to shut down VM %s.\n' "$TEMPLATE_ID"
  for _ in {1..200}; do
    status="$(ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n qm status "$TEMPLATE_ID" 2>/dev/null | awk '{print $2}')"
    if [[ "$status" == running ]]; then started=1; fi
    if [[ "$status" == stopped && "$started" == 1 ]]; then finished=1; break; fi
    if [[ "$status" == stopped && "$started" == 0 ]]; then say_error 'Windows VM stopped before setup could start'; fi
    if ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n grep -qF '[RA8FW-787] Bootstrap exception:' "$SERIAL_LOG_REMOTE_PATH" 2>/dev/null; then
      sleep 2
      # Read the exception straight from the host-side log the grep matched, so
      # its text reaches the operator even if the saved copy loses the tail.
      local exception_text
      exception_text="$(ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n grep -F -m1 -A4 '[RA8FW-787] Bootstrap exception:' "$SERIAL_LOG_REMOTE_PATH" 2>/dev/null | tr -d '\r' | head -c 4000 || true)"
      printf 'Windows bootstrap exception from the serial log:\n%s\n' "${exception_text:-<could not re-read the exception line>}" >&2
      capture_timeout_agent_diagnostics
      capture_timeout_screenshot
      say_error 'Windows bootstrap reported an exception; see the exception text above, the serial log and timeout screendump'
    fi
    sleep 15
  done
  if ((finished == 0)); then
    capture_timeout_agent_diagnostics
    capture_timeout_screenshot
    say_error 'Windows setup did not complete within the 50 minute limit; see the saved timeout screendump'
  fi
  capture_serial_log || say_error 'could not save the build serial log'
  remote_root "$TEMPLATE_ID" "$TEMPLATE_NAME" "$POOL_ID" "$STORAGE_ID" <<'REMOTE'
set -euo pipefail
vmid="$1"; expected_name="$2"; pool="$3"; storage="$4"
  [[ "$vmid" == 9012 && "$expected_name" == ra8-lab-windows-template && "$pool" == ra8-tf-lab && "$storage" == ra8-tf-lab ]] || { echo 'refusing to seal unexpected template identity' >&2; exit 1; }
config="$(qm config "$vmid")"
[[ "$config" == *'name: ra8-lab-windows-template'* && "$config" == *'RA8_LAB_TEMPLATE=windows-ci-v1'* && "$config" == *'sata0: ra8-tf-lab:'* ]] || { echo 'refusing to seal: VM identity or disk boundary mismatch' >&2; exit 1; }
[[ "$(qm status "$vmid" | awk '{print $2}')" == stopped ]] || { echo 'refusing to seal a running VM' >&2; exit 1; }
qm set "$vmid" --delete ide0,ide1,ide3,serial0 >/dev/null
config="$(qm config "$vmid")"
[[ "$config" != *$'\nserial0: '* && "$config" != serial0:* ]] || { echo 'refusing to seal template with build serial port attached' >&2; exit 1; }
qm set "$vmid" --boot order=sata0 >/dev/null
rm -f -- "/var/lib/vz/template/iso/unattend-${vmid}.iso"
qm template "$vmid"
qm set "$vmid" --ide2 "$storage:cloudinit" --citype configdrive2 --agent enabled=1 --pool "$pool" >/dev/null
qm set "$vmid" --protection 1 >/dev/null
REMOTE
  created=0
  trap - EXIT
  rm -rf -- "$run_dir"
  printf 'Created protected Server 2025 template %s with Cloudbase-Init %s, QEMU guest agent, and configdrive2.\n' "$TEMPLATE_ID" "$CLOUDBASE_VERSION"
}

main "$@"

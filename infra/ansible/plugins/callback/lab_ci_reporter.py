# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

from __future__ import annotations

DOCUMENTATION = r'''
    name: lab_ci_reporter
    type: stdout
    short_description: Disposable CI runner stdout reporter with live guest metrics
    description:
        - Extends the default Ansible stdout callback with real-time guest system metrics (load, cpu, ram, disk) during async polling.
    extends_documentation_fragment:
        - default_callback
        - result_format_callback
    requirements:
        - set as stdout in configuration
'''

import subprocess

from ansible import constants as C
from ansible.plugins.callback.default import CallbackModule as DefaultCallback


class CallbackModule(DefaultCallback):
    CALLBACK_VERSION = 2.0
    CALLBACK_TYPE = 'stdout'
    CALLBACK_NAME = 'lab_ci_reporter'

    def _sample_guest_metrics(self, result) -> str | None:
        try:
            host_obj = getattr(result, '_host', None)
            if not host_obj:
                return None
            hvars = host_obj.get_vars() if hasattr(host_obj, 'get_vars') else getattr(host_obj, 'vars', {})
            ansible_host = hvars.get('ansible_host', host_obj.get_name())
            ansible_port = str(hvars.get('ansible_port', 22))
            ansible_user = hvars.get('ansible_user')
            key_file = hvars.get('ansible_ssh_private_key_file') or hvars.get('ansible_private_key_file')
            shell_type = str(hvars.get('ansible_shell_type', '')).lower()

            ssh_cmd = [
                'ssh',
                '-o', 'BatchMode=yes',
                '-o', 'IdentitiesOnly=yes',
                '-o', 'StrictHostKeyChecking=no',
                '-o', 'UserKnownHostsFile=/dev/null',
                '-o', 'LogLevel=ERROR',
                '-o', 'ConnectTimeout=3',
                '-p', ansible_port,
            ]
            if key_file:
                ssh_cmd.extend(['-i', str(key_file)])

            target = f"{ansible_user}@{ansible_host}" if ansible_user else ansible_host
            ssh_cmd.append(target)

            if 'powershell' in shell_type:
                win_cmd = (
                    "powershell.exe -NoProfile -NonInteractive -Command "
                    "\"$os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue; "
                    "$tot = if ($os) { [math]::Round($os.TotalVisibleMemorySize/1MB,1) } else { 0 }; "
                    "$free = if ($os) { [math]::Round($os.FreePhysicalMemory/1MB,1) } else { 0 }; "
                    "$used = [math]::Round($tot - $free, 1); "
                    "$rp = if ($tot -gt 0) { [math]::Round($used/$tot*100) } else { 0 }; "
                    "$d = Get-PSDrive C -ErrorAction SilentlyContinue; "
                    "$fd = if ($d) { [math]::Round($d.Free/1GB, 1) } else { 0 }; "
                    "$td = if ($d) { [math]::Round(($d.Used+$d.Free)/1GB, 1) } else { 0 }; "
                    "$dp = if ($td -gt 0) { [math]::Round($fd/$td*100) } else { 0 }; "
                    "$cpu = (Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Measure-Object -Property LoadPercentage -Average).Average; "
                    "$cpus = [Environment]::ProcessorCount; "
                    "$load = if ($cpu) { '{0:N2}' -f ($cpu * $cpus / 100) } else { '0.00' }; "
                    "$top = (Get-Process | Where-Object { $_.Name -notin @('Idle', 'System', 'svchost', 'powershell', 'sshd', 'WmiPrvSE', 'sihost', 'taskhostw') } | Sort-Object CPU -Descending | Select-Object -First 1).Name; "
                    "$task = if ($top) { ' | active: ' + $top } else { '' }; "
                    "Write-Host ('load: [{0}] | cpu: {1}% | ram: {2}G/{3}G ({4}%) | disk: {5}G free ({6}%){7}' -f $load, $cpu, $used, $tot, $rp, $fd, $dp, $task)\""
                )
                ssh_cmd.append(win_cmd)
            else:
                linux_cmd = (
                    "python3 -c '"
                    "import os,time,shutil,subprocess; "
                    "l = \", \".join(f\"{x:.2f}\" for x in os.getloadavg()); "
                    "t,u,f = shutil.disk_usage(\"/\"); "
                    "m = {k.rstrip(\":\"): int(v) for k, v in (x.split()[:2] for x in open(\"/proc/meminfo\"))}; "
                    "tot = m.get(\"MemTotal\", 0) / 1048576; "
                    "av = m.get(\"MemAvailable\", 0) / 1048576; "
                    "used = tot - av; "
                    "rp = (used / tot * 100) if tot else 0; "
                    "c1 = [int(x) for x in open(\"/proc/stat\").readline().split()[1:5]]; "
                    "time.sleep(0.05); "
                    "c2 = [int(x) for x in open(\"/proc/stat\").readline().split()[1:5]]; "
                    "db = (c2[0] + c2[1] + c2[2]) - (c1[0] + c1[1] + c1[2]); "
                    "dt = sum(c2) - sum(c1); "
                    "cpu = f\"{(db / dt * 100):.0f}%\" if dt > 0 else \"0%\"; "
                    "top_cmd = subprocess.run(\"ps -eo comm --sort=-pcpu | awk \\\"NR>1 && !/^(ps|python3|awk|sshd|systemd|kworker|bash|sh|init|tmux)/ {print; exit}\\\"\", shell=True, capture_output=True, text=True).stdout.strip(); "
                    "task = f\" | active: {top_cmd}\" if top_cmd else \"\"; "
                    "print(f\"load: [{l}] | cpu: {cpu} | ram: {used:.1f}G/{tot:.1f}G ({rp:.0f}%) | disk: {f/1073741824:.1f}G free ({f/t*100:.0f}%){task}\")"
                    "'"
                )
                ssh_cmd.append(linux_cmd)

            proc = subprocess.run(ssh_cmd, capture_output=True, text=True, timeout=8.0)
            if proc.returncode == 0:
                out = proc.stdout.strip()
                return out or None
        except Exception:
            pass
        return None

    def v2_runner_on_async_poll(self, result):
        host = result._host.get_name()
        jid = result._result.get('ansible_job_id')
        started = result._result.get('started')
        finished = result._result.get('finished')
        metrics = result._result.get('system_metrics') or self._sample_guest_metrics(result)
        if metrics:
            msg = f"ASYNC POLL on {host}: jid={jid} started={started} finished={finished} | {metrics}"
        else:
            msg = f"ASYNC POLL on {host}: jid={jid} started={started} finished={finished}"
        self._display.display(msg, color=C.COLOR_DEBUG)

    def v2_runner_retry(self, result: object) -> None:
        """Expose incremental Windows guest log output during until retries."""
        super().v2_runner_retry(result)
        result_data = getattr(result, 'result', None)
        if result_data is None:
            result_data = getattr(result, '_result', {})
        host_obj = getattr(result, 'host', None) or getattr(result, '_host', None)
        host = host_obj.get_name() if host_obj is not None else 'unknown'
        for stream in ('stdout_lines', 'stderr_lines'):
            lines = result_data.get(stream) or []
            for line in lines:
                self._display.display(f"[{host} ci {stream}] {line}")

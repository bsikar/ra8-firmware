# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

from __future__ import annotations

import os
from ansible.plugins.action import ActionBase
from ansible.utils.vars import merge_hash


class ActionModule(ActionBase):

    def _get_async_dir(self):
        async_dir = self.get_shell_option('async_dir', default="~/.ansible_async")
        return self._remote_expand_user(async_dir)

    def _collect_linux_metrics(self):
        cmd = (
            "python3 -c \""
            "import os, time, shutil; "
            "try:\n"
            "    load = ', '.join(f'{x:.2f}' for x in os.getloadavg())\n"
            "except Exception:\n"
            "    load = 'N/A'\n"
            "try:\n"
            "    mem = {}\n"
            "    with open('/proc/meminfo') as f:\n"
            "        for l in f:\n"
            "            p = l.split()\n"
            "            if len(p) >= 2: mem[p[0].rstrip(':')] = int(p[1])\n"
            "    tot = mem.get('MemTotal', 0) / 1048576\n"
            "    avail = mem.get('MemAvailable', 0) / 1048576\n"
            "    used = tot - avail\n"
            "    pct = (used / tot * 100) if tot > 0 else 0\n"
            "    ram = f'{used:.1f}G/{tot:.1f}G ({pct:.0f}%)'\n"
            "except Exception:\n"
            "    ram = 'N/A'\n"
            "try:\n"
            "    total, used_d, free = shutil.disk_usage('/')\n"
            "    free_g = free / 1073741824\n"
            "    free_pct = (free / total * 100) if total > 0 else 0\n"
            "    disk = f'{free_g:.1f}G free ({free_pct:.0f}%)'\n"
            "except Exception:\n"
            "    disk = 'N/A'\n"
            "try:\n"
            "    with open('/proc/stat') as f:\n"
            "        c1 = [int(x) for x in f.readline().split()[1:5]]\n"
            "    time.sleep(0.05)\n"
            "    with open('/proc/stat') as f:\n"
            "        c2 = [int(x) for x in f.readline().split()[1:5]]\n"
            "    d_busy = (c2[0] + c2[1] + c2[2]) - (c1[0] + c1[1] + c1[2])\n"
            "    d_tot = sum(c2) - sum(c1)\n"
            "    cpu = f'{(d_busy / d_tot * 100):.0f}%' if d_tot > 0 else '0%'\n"
            "except Exception:\n"
            "    cpu = 'N/A'\n"
            "print(f'load: [{load}] | cpu: {cpu} | ram: {ram} | disk: {disk}')\n"
            "\" 2>/dev/null"
        )
        res = self._low_level_execute_command(cmd, sudo=False)
        out = res.get('stdout', '').strip()
        return out if out else None

    def _collect_windows_metrics(self):
        cmd = (
            "powershell.exe -NoProfile -NonInteractive -Command \""
            "$os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue; "
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
            "Write-Host ('load: [{0}] | cpu: {1}% | ram: {2}G/{3}G ({4}%) | disk: {5}G free ({6}%)' -f $load, $cpu, $used, $tot, $rp, $fd, $dp)"
            "\""
        )
        res = self._low_level_execute_command(cmd, sudo=False)
        out = res.get('stdout', '').strip()
        return out if out else None

    def run(self, tmp=None, task_vars=None):
        results = super(ActionModule, self).run(tmp, task_vars)

        validation_result, new_module_args = self.validate_argument_spec(
            argument_spec={
                'jid': {'type': 'str', 'required': True},
                'mode': {'type': 'str', 'choices': ['status', 'cleanup'], 'default': 'status'},
            },
        )

        results['started'] = results['finished'] = False
        results['stdout'] = results['stderr'] = ''
        results['stdout_lines'] = results['stderr_lines'] = []

        jid = new_module_args["jid"]
        mode = new_module_args["mode"]

        results['ansible_job_id'] = jid
        async_dir = self._get_async_dir()
        log_path = self._connection._shell.join_path(async_dir, jid)

        if mode == 'cleanup':
            results['erased'] = log_path
        else:
            results['results_file'] = log_path
            results['started'] = True

        new_module_args['_async_dir'] = async_dir
        results = merge_hash(
            results,
            self._execute_module(
                module_name='ansible.legacy.async_status',
                task_vars=task_vars,
                module_args=new_module_args
            )
        )

        for convert in ('started', 'finished'):
            results[convert] = bool(results[convert])

        # If the job is active, sample live guest system metrics
        if results.get('started') and not results.get('finished'):
            try:
                shell_type = getattr(self._connection, '_shell', None)
                shell_name = getattr(shell_type, 'SHELL_FAMILY', '') or getattr(shell_type, '_SHELL_FAMILY', '')
                if 'powershell' in str(shell_name).lower():
                    metrics = self._collect_windows_metrics()
                else:
                    metrics = self._collect_linux_metrics()
                if metrics:
                    results['system_metrics'] = metrics
            except Exception:
                pass

        return results

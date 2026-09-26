#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Expose one fixed remote loopback target through ordinary SSH stdio.

The Proxmox SSH service disables TCP forwarding. This small local proxy keeps
that policy intact: each localhost connection gets an SSH command channel to
the exact approved `nc` target, with no user-controlled remote command or
remote address.
"""

from __future__ import annotations

import argparse
import os
import socket
import socketserver
import subprocess
import threading

SSH_ALIAS = "pve"
TARGETS = {
    "api": ("127.0.0.1", "8006"),
    "guest": ("10.250.9.10", "22"),
    "guest_windows": ("10.250.8.20", "22"),
    "guest_windows_winrm": ("10.250.8.20", "5985"),
}


def copy_socket_to_child(connection: socket.socket, child: subprocess.Popen[bytes]) -> None:
    try:
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                break
            assert child.stdin is not None
            child.stdin.write(chunk)
            child.stdin.flush()
    finally:
        if child.stdin is not None:
            child.stdin.close()


def copy_child_to_socket(connection: socket.socket, child: subprocess.Popen[bytes]) -> None:
    try:
        assert child.stdout is not None
        while True:
            chunk = os.read(child.stdout.fileno(), 65536)
            if not chunk:
                break
            connection.sendall(chunk)
    finally:
        try:
            connection.shutdown(socket.SHUT_WR)
        except OSError:
            pass


class ProxyHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        target_host, target_port = self.server.target  # type: ignore[attr-defined]
        child = subprocess.Popen(
            [
                "ssh",
                "-T",
                "-o",
                "BatchMode=yes",
                "-o",
                "RequestTTY=no",
                SSH_ALIAS,
                "/usr/bin/nc",
                target_host,
                target_port,
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        to_child = threading.Thread(
            target=copy_socket_to_child, args=(self.request, child), daemon=True
        )
        from_child = threading.Thread(
            target=copy_child_to_socket, args=(self.request, child), daemon=True
        )
        to_child.start()
        from_child.start()
        while to_child.is_alive() and from_child.is_alive() and child.poll() is None:
            to_child.join(timeout=0.2)
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                child.kill()
        to_child.join(timeout=1.0)
        from_child.join(timeout=1.0)


class ProxyServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, port: int, target: tuple[str, str]) -> None:
        super().__init__(("127.0.0.1", port), ProxyHandler)
        self.target = target


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--target", choices=sorted(TARGETS), required=True)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("--port must be an unprivileged local port")
    with ProxyServer(args.port, TARGETS[args.target]) as server:
        server.serve_forever()


if __name__ == "__main__":
    main()

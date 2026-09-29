#!/usr/bin/env python3
import sys
import socket
import time

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/var/run/qemu-server/9011.qmp')
s.recv(1024)
s.sendall(b'{"execute": "qmp_capabilities"}\n')
s.recv(1024)

cmd = sys.argv[1] + '\n'

KEY_MAP = {
    ':': [{'type': 'qcode', 'data': 'shift'}, {'type': 'qcode', 'data': 'semicolon'}],
    '\\': [{'type': 'qcode', 'data': 'backslash'}],
    ' ': [{'type': 'qcode', 'data': 'spc'}],
    '/': [{'type': 'qcode', 'data': 'slash'}],
    '.': [{'type': 'qcode', 'data': 'dot'}],
    '>': [{'type': 'qcode', 'data': 'shift'}, {'type': 'qcode', 'data': 'dot'}],
    '&': [{'type': 'qcode', 'data': 'shift'}, {'type': 'qcode', 'data': '7'}],
    '"': [{'type': 'qcode', 'data': 'shift'}, {'type': 'qcode', 'data': 'apostrophe'}],
    '=': [{'type': 'qcode', 'data': 'equal'}],
    '|': [{'type': 'qcode', 'data': 'shift'}, {'type': 'qcode', 'data': 'backslash'}],
    '\n': [{'type': 'qcode', 'data': 'ret'}],
    '-': [{'type': 'qcode', 'data': 'minus'}],
    '_': [{'type': 'qcode', 'data': 'shift'}, {'type': 'qcode', 'data': 'minus'}],
}

for ch in cmd:
    if ch in KEY_MAP:
        keys = KEY_MAP[ch]
    elif ch.isupper():
        keys = [{'type': 'qcode', 'data': 'shift'}, {'type': 'qcode', 'data': ch.lower()}]
    else:
        keys = [{'type': 'qcode', 'data': ch}]
    msg = f'{{"execute": "send-key", "arguments": {{"keys": {keys}}}}}\n'.replace("'", '"')
    s.sendall(msg.encode())
    s.recv(1024)
    time.sleep(0.015)

time.sleep(1.0)
s.sendall(b'{"execute": "screendump", "arguments": {"filename": "/tmp/screen9011.ppm"}}\n')
s.recv(1024)

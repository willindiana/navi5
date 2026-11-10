#!/usr/bin/env python3
# Navi5 — UDP receiver: discard all except strict numeric diagnostic rows.
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import re
import socket

ROW = re.compile(r'(?:ok=[01] failure=\d+ curl=-?\d+ http=-?\d+/-?\d+ os=-?\d+/-?\d+ lookup=-?\d+\.\d+/-?\d+ connect=-?\d+\.\d+/-?\d+ total=-?\d+\.\d+/-?\d+ trace=\d+/\d+ first=-?\d+/-?\d+/-?\d+/-?\d+ log=-?[01]|sock=\d+/-?\d+/-?\d+/-?\d+/-?\d+)')


def numeric(data):
    try:
        text = data.decode('ascii').rstrip('\n')
    except UnicodeError:
        return None
    # Direct DEV sink has no EVO envelope; strict numeric-only schema.
    if re.fullmatch(r'live=\d+/[0-4]/\d+/-?\d+/-?\d+/-?\d+/-?\d+', text):
        return text
    # EVO adds timestamp and evo_bt prefix; accept entire envelope, not substrings.
    match = re.fullmatch(r'\[\d+\.\d{3}\] EVO boot: (.*)', text)
    return match[1] if match and ROW.fullmatch(match[1]) else None


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port', type=int, default=5555)
    a = p.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('', a.port))
        while True:
            data = s.recv(4096)
            row = numeric(data)
            if row is not None:
                print(row, flush=True)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass

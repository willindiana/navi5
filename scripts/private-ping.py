#!/usr/bin/env python3
# Navi5 — compile shared sources; private values never passed in argv.
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = Path.home() / 'Library/Application Support/Navi5/dev-login.json'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', type=Path, default=DEFAULT)
    parser.add_argument('--allow-http', action='store_true')
    args = parser.parse_args()
    if not args.config.is_file():
        print('config=missing auth=pending')
        return 2
    try:
        with tempfile.TemporaryDirectory(prefix='navi5-ping-') as tmp:
            obj, binary = Path(tmp) / 'json.o', Path(tmp) / 'ping'
            def run(cmd):
                return subprocess.run(cmd, capture_output=True, check=True)
            flags = shlex.split(run(['pkg-config', '--cflags', '--libs', 'libavutil']).stdout.decode())
            run(['clang', '-c', str(ROOT / 'app/engine/addons/src/cJSON.c'),
                 '-I' + str(ROOT / 'app/engine/addons/include'), '-o', str(obj)])
            run(['clang++', '-std=c++17', '-O1', '-I' + str(ROOT / 'app/src'),
                 '-I' + str(ROOT / 'app/src/jf'), '-I' + str(ROOT / 'app/engine/addons/include'),
                 '-DNAVI5_CA_BUNDLE="' + str(ROOT / 'app/assets/ca-bundle.pem') + '"',
                 str(ROOT / 'app/tests/host/private_ping.cpp'), str(ROOT / 'app/src/subsonic/subsonic.cpp'),
                 str(ROOT / 'app/tests/host/jf_http_curl.cpp'), str(obj), *flags, '-lcurl', '-o', str(binary)])
            result = subprocess.run([str(binary), str(args.config), '--allow-http' if args.allow_http else '--https-only'],
                                    capture_output=True)
            # Emit only driver-generated fields. Child stderr can include runtime details.
            text = result.stdout.decode('ascii', errors='strict')
            if len(text) > 512 or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789= -./\n' for c in text):
                print('runner=invalid-output'); return 2
            print(text, end='')
            return result.returncode
    except (OSError, subprocess.SubprocessError, UnicodeError):
        print('runner=failed auth=pending')
        return 2


if __name__ == '__main__':
    raise SystemExit(main())

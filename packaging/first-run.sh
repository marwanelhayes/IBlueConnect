#!/bin/sh
set -eu
libdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$libdir/setup-venv/bin/python3" "$libdir/setup-user.py" "$libdir" "$@"

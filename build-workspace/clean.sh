#!/bin/sh
# Compatibility entry point; preserves tracked files, including build metadata.
exec python3 "$(dirname "$0")/../scripts/clean-build.py" "$@"

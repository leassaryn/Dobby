#!/bin/sh
# Compatibility entry point; all build/package logic lives in Python.
exec python3 "$(dirname "$0")/../scripts/build-android.py" --prefab "$@"

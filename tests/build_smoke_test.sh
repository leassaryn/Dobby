#!/bin/sh
# Build + run the Dobby core smoke test against the host-built libdobby.a.
# Usage (from repo root): sh tests/build_smoke_test.sh
set -e

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO_ROOT/tests/build/core_smoke_test"

# Build the host library if it isn't present yet.
if [ ! -f "$REPO_ROOT/build/libdobby.a" ]; then
  echo "[*] Building host libdobby.a..."
  mkdir -p "$REPO_ROOT/build" && cd "$REPO_ROOT/build" && cmake .. >/dev/null && make -j4 dobby >/dev/null
fi

mkdir -p "$REPO_ROOT/tests/build"

CXX="${CXX:-clang++}"
$CXX -std=c++23 -g \
  "$REPO_ROOT/tests/core_smoke_test.cc" \
  -I"$REPO_ROOT/include" \
  "$REPO_ROOT/build/libdobby.a" \
  -llog -lpthread -ldl \
  -o "$OUT"

echo "[*] Running core smoke test..."
"$OUT"

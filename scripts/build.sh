#!/bin/sh
# One-command local build: compile the arm64 camera binaries (via Docker), then pack the .b3.
# This is the convenient entry point; it just chains the two steps CI also runs:
#   1. toolchain/build.sh  -> compiles src/ into toolchain/dist/ (needs Docker; arm64, emulated on
#      a non-arm host)
#   2. scripts/pack.sh      -> stages the binaries and zips dist/camera-hw-accel-<version>.b3
# The index atom (scripts/generate-atom.mjs) is produced by CI with the real release URL; run it
# by hand only for a local dry run.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

echo "==> [1/2] Building arm64 binaries (Docker)"
sh "$REPO_DIR/toolchain/build.sh"

echo "==> [2/2] Packing the .b3"
sh "$SCRIPT_DIR/pack.sh"

echo "==> Done. The .b3 is in dist/."

#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (C) 2026 unlucio and the Bespok3d contributors
# SPDX-License-Identifier: GPL-3.0-only
# This plugin's own gate: it must pass from this repo's root, with no sibling repo cloned except
# lib_bespok3d. Exits non-zero on any failure.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The shared gate helpers and the detectors that enforce a workspace-wide rule live in one place.
# See lib_bespok3d/tooling/README.md. This is the only line that knows where they are.
B3D_TOOLING="${B3D_TOOLING:-$REPO_ROOT/lib_bespok3d/tooling}"
# lib_bespok3d is a submodule. A clone made without it leaves an empty directory here, so say what
# is actually wrong instead of letting every check below fail on a missing file.
if [ ! -f "$B3D_TOOLING/gate-lib.sh" ] || [ ! -f "$B3D_TOOLING/release-trigger-detector.mjs" ]; then
    echo "The shared gate helpers are missing or older than the checks this gate runs:" >&2
    echo "the lib_bespok3d submodule is not checked out, or is pinned to an older commit." >&2
    echo "Run this once from the repo root, then try again:" >&2
    echo "  git submodule sync --recursive && git submodule update --init --recursive" >&2
    echo "See CONTRIBUTING.md for the full environment setup." >&2
    exit 1
fi

# shellcheck source=/dev/null
. "$B3D_TOOLING/gate-lib.sh"

cd "$REPO_ROOT" || exit 1

echo ""
echo "u1-hw-camera gate"

b3d_python_tools

run_check "pytest"  pytest_in_dir "$REPO_ROOT" tests
# The MPP stream server is the only Python that is ours; the rest of src/ is C and build glue.
run_check "ruff"    ruff_in_dir "$REPO_ROOT" src/v4l2-mpp/apps/stream-http/camera-stream.py tests

release_trigger_check "$REPO_ROOT"
workflow_pinning_check "$REPO_ROOT"
em_dash_check "$REPO_ROOT"
shellcheck_repo "$REPO_ROOT"


# Per-file REUSE compliance: every file is covered by a copyright and licence statement, its own
# header or the REUSE.toml block, and every licence a file names has its text in LICENSES/.
# Whole-project `reuse lint` is not used here, because LICENSES/ also carries the texts for
# third-party code that the built package conveys but that is not committed in this repo (see
# REUSE.toml), which that mode reports as unused. The file list is tracked plus not-yet-committed
# files, so a newly vendored file is checked before it is committed rather than after, and a
# not-yet-committed rename does not point the linter at a path that no longer exists. `reuse` is not
# a workspace dependency: an installed one is used when present, otherwise uv runs it from cache, and
# a machine with neither reports the check as skipped rather than as passed.
# shellcheck disable=SC2329  # run_check invokes this by name, which shellcheck cannot follow.
run_reuse_lint() {
    if command -v reuse > /dev/null 2>&1; then
        reuse "$@"
    else
        uvx --quiet --from 'reuse[charset-normalizer]' reuse "$@"
    fi
}

# shellcheck disable=SC2329  # run_check invokes this by name, which shellcheck cannot follow.
reuse_per_file_check() {
    local licensed_paths=()
    local candidate_path
    local licensed_count=0
    while IFS= read -r -d '' candidate_path; do
        if [ -f "$candidate_path" ]; then
            licensed_paths+=("$candidate_path")
            licensed_count=$((licensed_count + 1))
        fi
    done < <(git ls-files -z --cached --others --exclude-standard)
    if [ "$licensed_count" -eq 0 ]; then
        return 0
    fi
    run_reuse_lint lint-file "${licensed_paths[@]}"
}

if command -v reuse > /dev/null 2>&1 || command -v uvx > /dev/null 2>&1; then
    run_check "reuse (per-file licensing)" reuse_per_file_check
else
    skip_check "reuse (per-file licensing)" "install reuse, or install uv so it can be run from cache"
fi

gate_summary || exit 1

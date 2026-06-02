#!/bin/sh
# Build the camera-hw-accel .b3 from plugin/ plus the binaries built into toolchain/dist/.
# A slim port of the monorepo's pack-plugins.sh pack_plugin. This plugin is always-repack:
# its binaries are build outputs, so there is no content-hash/lockfile/auto-bump here; bump
# plugin/manifest.json version manually to cut a new release.
#
# Requires: zip, jq, and shasum (macOS) or sha256sum (Linux). Run toolchain/build.sh first.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
PLUGIN_DIR="$REPO_DIR/plugin"
TOOLCHAIN_DIST="$REPO_DIR/toolchain/dist"
DIST_DIR="$REPO_DIR/dist"
BIN_DIR="$PLUGIN_DIR/files/bin"

for cmd in zip jq; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "ERROR: '$cmd' is required." >&2; exit 1; }
done
command -v shasum >/dev/null 2>&1 || command -v sha256sum >/dev/null 2>&1 \
  || { echo "ERROR: shasum or sha256sum is required." >&2; exit 1; }

file_sha256() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else sha256sum "$1" | awk '{print $1}'; fi
}

file_mode() { stat -f "%OLp" "$1" 2>/dev/null || stat -c "%a" "$1" 2>/dev/null; }

stage_binaries() {
  mkdir -p "$BIN_DIR"
  missing=""
  for f in capture-v4l2-jpeg-mpp capture-v4l2-raw-mpp stream-webrtc stream-rtsp \
           stream-http.py fake-service libv4l2-imposter.so; do
    if [ -f "$TOOLCHAIN_DIST/$f" ]; then cp "$TOOLCHAIN_DIST/$f" "$BIN_DIR/"
    else missing="$missing $f"; fi
  done
  if [ -n "$missing" ]; then
    echo "ERROR: missing built binaries in toolchain/dist/:$missing" >&2
    echo "       Run toolchain/build.sh first." >&2
    exit 1
  fi
}

# LC_ALL=C forces a byte-order sort so the file list is identical regardless of locale.
build_files_array() {
  find "$PLUGIN_DIR/files" -type f \
    ! -path '*/__pycache__/*' ! -name '*.pyc' ! -name '.DS_Store' \
    | LC_ALL=C sort | while read -r fpath; do
    relpath="${fpath#"$PLUGIN_DIR/"}"
    sha=$(file_sha256 "$fpath")
    mode=$(file_mode "$fpath")
    case "$mode" in *7*) mode="755" ;; *) mode="644" ;; esac
    printf '{"path":"%s","sha256":"%s","mode":"%s"}\n' "$relpath" "$sha" "$mode"
  done
}

stage_binaries

version=$(jq -r '.version' "$PLUGIN_DIR/manifest.json")
output="$DIST_DIR/camera-hw-accel-$version.b3"
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

files_json=$(build_files_array | jq -s '.')
jq --argjson files "$files_json" '.files = $files' "$PLUGIN_DIR/manifest.json" > "$tmp_dir/manifest.json"

mkdir -p "$DIST_DIR"
rm -f "$output"
(
  cd "$PLUGIN_DIR"
  zip -qr "$output" files/
  if [ -d doc ]; then zip -qr "$output" doc/; fi
  cd "$tmp_dir"
  zip -q "$output" manifest.json
)

echo "Packed: $output"
echo "  sha256: $(file_sha256 "$output")"

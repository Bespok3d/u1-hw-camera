#!/bin/sh
# Regenerate the two splash screens a camera viewer sees in place of live video, so the tile always
# carries a message instead of a frozen frame or a black rectangle:
#
#   connecting   "Connecting..."       shown while the stream is still coming up for that viewer
#   interrupted  "Stream interrupted"  shown once a viewer's live stream stops
#
# Each one produces TWO artifacts from one rendered message image:
#   - <name>.jpg     for the MJPEG stream + the snapshot endpoint (read by camera-stream.py)
#   - <name>_h264.h  a one-frame annexb H264 keyframe (SPS+PPS+IDR) compiled into stream-webrtc
#
# The outputs are committed, NOT built in the arm64 toolchain: the text is fixed and universal and the
# encoded bytes are architecture-independent (a JPEG and an H264 elementary stream decode identically
# everywhere), so baking ffmpeg/libx264 into the QEMU-emulated Docker build would only add weight and
# ship bytes nobody decode-tested. Run this on a dev host (needs ImageMagick + ffmpeg with libx264),
# eyeball the JPEGs, and confirm the H264 decodes in Chromium + WebKit before committing.
set -e

ASSETS_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$ASSETS_DIR")"

SHIPPED_HTML="$REPO_DIR/plugin/files/html"
SOURCE_HTML="$REPO_DIR/src/v4l2-mpp/apps/stream-http/html"
WEBRTC_DIR="$REPO_DIR/src/v4l2-mpp/apps/stream-webrtc"

WIDTH=1280
HEIGHT=720
BACKGROUND="#1b1f24"
TITLE_COLOR="#e8eaed"
SUBTITLE_COLOR="#9aa3ad"

for cmd in magick ffmpeg xxd; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "ERROR: '$cmd' is required." >&2; exit 1; }
done

# A sans-serif font, resolved portably: an explicit SPLASH_FONT (name or path) wins, else the
# first system font file that exists on this host (macOS / common Linux), so the script runs without
# arguments on either.
resolve_font() {
  if [ -n "$SPLASH_FONT" ]; then echo "$SPLASH_FONT"; return; fi
  for candidate in \
    /System/Library/Fonts/Supplemental/Arial.ttf \
    /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
    /usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf; do
    [ -f "$candidate" ] && { echo "$candidate"; return; }
  done
  echo "ERROR: no font found; set SPLASH_FONT to a .ttf path." >&2
  exit 1
}
SPLASH_FONT="${SPLASH_FONT:-}"
FONT="$(resolve_font)"

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

# Normalize every NAL to a 4-byte start code. x264 emits a 3-byte (00 00 01) code before the IDR
# slice, but both the capture feed (h264_frames.h only matches 00 00 00 01) and the RTP packetizer
# (Separator::LongStartSequence) split on 4-byte codes only, so a 3-byte-prefixed NAL would be
# mis-split and reach the browser malformed.
normalize_start_codes() {
  python3 - "$1" "$2" <<'PY'
import sys

data = open(sys.argv[1], "rb").read()
starts = []
index = 0
while index < len(data) - 2:
    if data[index] == 0 and data[index + 1] == 0 and data[index + 2] == 1:
        starts.append(index)
        index += 3
    else:
        index += 1

out = bytearray()
for position, start in enumerate(starts):
    payload_start = start + 3
    payload_end = starts[position + 1] if position + 1 < len(starts) else len(data)
    if payload_end < len(data) and data[payload_end - 1] == 0:
        payload_end -= 1  # the trailing 0x00 belongs to the next NAL's 4-byte start code
    out += b"\x00\x00\x00\x01" + data[payload_start:payload_end]

open(sys.argv[2], "wb").write(out)
PY
}

# One splash: name (the asset stem and the C symbol), the big line, the small line, and the sentence
# that tells a reader of the generated header when a viewer sees it.
render_splash() {
  splash_name="$1"
  splash_title="$2"
  splash_subtitle="$3"
  splash_when="$4"

  master_png="$work_dir/$splash_name.png"
  master_h264="$work_dir/$splash_name.h264"
  header="$WEBRTC_DIR/${splash_name}_h264.h"

  echo "==> $splash_name: rendering message image (${WIDTH}x${HEIGHT})"
  magick -size "${WIDTH}x${HEIGHT}" "canvas:${BACKGROUND}" \
    -gravity center \
    -font "$FONT" -pointsize 72 -fill "$TITLE_COLOR" -annotate +0-40 "$splash_title" \
    -font "$FONT" -pointsize 38 -fill "$SUBTITLE_COLOR" -annotate +0+55 "$splash_subtitle" \
    "$master_png"

  echo "==> $splash_name: writing $splash_name.jpg"
  magick "$master_png" -quality 90 "$SHIPPED_HTML/$splash_name.jpg"
  cp "$SHIPPED_HTML/$splash_name.jpg" "$SOURCE_HTML/$splash_name.jpg"

  # baseline profile + yuv420p + a single IDR keyframe = the broadest-compatibility decode path and a
  # self-contained frame, so a viewer whose live stream was a different size still re-inits on the SPS.
  echo "==> $splash_name: encoding one-frame annexb H264 keyframe"
  ffmpeg -y -loglevel error -loop 1 -i "$master_png" -frames:v 1 \
    -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
    -x264-params keyint=1:scenecut=0:annexb=1 \
    -f h264 "$work_dir/$splash_name-raw.h264"

  echo "==> $splash_name: normalizing NAL start codes to 4 bytes"
  normalize_start_codes "$work_dir/$splash_name-raw.h264" "$master_h264"

  echo "==> $splash_name: emitting ${splash_name}_h264.h"
  {
    echo "#pragma once"
    echo ""
    echo "#include <stdint.h>"
    echo "#include <stddef.h>"
    echo ""
    echo "// Pre-encoded H264 keyframe (annexb SPS+PPS+IDR) of the \"$splash_title\" message."
    echo "// A WebRTC viewer is sent this $splash_when,"
    echo "// so its tile carries a message instead of a frozen frame."
    echo "// Generated by assets/generate-splash.sh and committed: the bytes are fixed, universal, and"
    echo "// architecture-independent, so there is no build-time encoder dependency. Do not hand-edit;"
    echo "// rerun the generator instead."
    echo "static const uint8_t ${splash_name}_h264[] = {"
    xxd -i < "$master_h264"
    echo "};"
    echo "static const size_t ${splash_name}_h264_len = sizeof(${splash_name}_h264);"
  } > "$header"

  echo "    $SHIPPED_HTML/$splash_name.jpg ($(wc -c < "$SHIPPED_HTML/$splash_name.jpg") bytes), $header (H264 $(wc -c < "$master_h264") bytes)"
}

render_splash connecting "Connecting..." "Please wait, the camera is starting" \
  "while its stream is still coming up"
render_splash interrupted "Stream interrupted" "Refresh the page to restart it" \
  "once the live stream it was watching stops"

echo ""
echo "Done."

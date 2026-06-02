#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"   # the C sources live in src/ at the repo root
cd "$SCRIPT_DIR"
. "$SCRIPT_DIR/lib/docker-build.sh"

if ! command -v docker > /dev/null 2>&1; then
    echo "ERROR: docker not found"
    exit 1
fi

echo "==> Building camera binaries (arm64 via Docker emulation)..."
rm -rf dist
docker_build_base
# Context is the repo root so the Dockerfile's `COPY src/...` resolves; the .dockerignore there
# keeps only src/ in the context. dist/ still lands under toolchain/ (cwd), where pack.sh reads it.
docker_image bespok3d-camera-hw-accel "$SCRIPT_DIR/Dockerfile" "$REPO_DIR"

echo "==> Extracting artifacts to dist/..."
docker_extract bespok3d-camera-hw-accel bespok3d-camera-hw-accel-tmp
# Fallback for images where html wasn't in /out/
if [ -z "$(ls dist/html/ 2>/dev/null)" ]; then
    mkdir -p dist/html
    CONTAINER_ID=$(docker create --platform linux/arm64 bespok3d-camera-hw-accel)
    docker cp "${CONTAINER_ID}:/dist/usr/share/stream-http/html/." ./dist/html/ 2>/dev/null || true
    docker rm "${CONTAINER_ID}" > /dev/null
fi

echo "==> Validating output..."
MISSING=0
for binary in capture-v4l2-raw-mpp capture-v4l2-jpeg-mpp stream-webrtc stream-rtsp \
              stream-http.py control-v4l2.py \
              fake-service libv4l2-imposter.so; do
    if [ ! -f "dist/${binary}" ]; then
        echo "  MISSING binary: ${binary}"
        MISSING=1
    fi
done
if [ -z "$(ls dist/html/*.html 2>/dev/null)" ]; then
    echo "  MISSING: dist/html/*.html (stream-http WebRTC UI)"
    MISSING=1
fi

if [ "$MISSING" -eq 1 ]; then
    echo "ERROR: Some artifacts are missing. Check the Docker build output."
    exit 1
fi

echo ""
echo "Build complete:"
find dist -maxdepth 1 -type f -exec ls -lh {} +
echo "HTML: $(ls dist/html/)"

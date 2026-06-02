#!/bin/sh
# Docker helpers for the arm64 plugin build. The image targets linux/arm64 (the U1 is a Rockchip
# SoC); on a non-arm host this runs under QEMU emulation. When CI sets B3D_CACHE_ARGS it adds the
# buildx layer-cache flags, so unchanged layers (apt + v4l2-mpp deps) restore instead of rebuilding;
# unset locally, so a plain local build is unaffected.

# Build the plugin image from an explicit Dockerfile (arg 2) and build context (arg 3). --load
# brings the result into the local image store so docker_extract can copy from it.
docker_image() {
    # shellcheck disable=SC2086
    docker buildx build --platform linux/arm64 --load $B3D_CACHE_ARGS -t "$1" -f "$2" "$3"
}

# Copy an image's output dir (default /out) into ./dist (relative to the caller's cwd).
docker_extract() {
    mkdir -p dist
    docker create --name "$2" "$1" > /dev/null
    docker cp "$2:${3:-/out}/." ./dist/
    docker rm "$2" > /dev/null
}

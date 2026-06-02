#!/bin/sh
# Docker helpers for the arm64 plugin build. Every image targets linux/arm64 (the U1 is a Rockchip
# SoC); on a non-arm host this runs under QEMU emulation.
_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

# Build the shared toolchain base image (gcc / make / cmake / git / pkg-config).
docker_build_base() {
    docker build --platform linux/arm64 -t bespok3d-build-base \
        -f "$_LIB_DIR/Dockerfile.base" "$_LIB_DIR"
}

# Build an image from an explicit Dockerfile (arg 2) and build context (arg 3), so the context can
# sit apart from the Dockerfile (here: repo root, because the C sources live in src/ at the root).
docker_image() {
    docker build --pull=false --platform linux/arm64 -t "$1" -f "$2" "$3"
}

# Copy an image's output dir (default /out) into ./dist (relative to the caller's cwd).
docker_extract() {
    mkdir -p dist
    docker create --name "$2" "$1" > /dev/null
    docker cp "$2:${3:-/out}/." ./dist/
    docker rm "$2" > /dev/null
}

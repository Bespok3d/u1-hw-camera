# Single stage on purpose: the whole toolchain (apt packages + the v4l2-mpp `make deps`) is one
# cacheable layer chain, so a GitHub Actions layer cache restores it and only the app COPY + `make
# install` layers rebuild when our code changes. Targets linux/arm64 (Rockchip U1); emulated under
# QEMU on an x86 runner.
#
# We own the v4l2-mpp source now (vendored under src/v4l2-mpp, see src/v4l2-mpp/VENDORING.md), so we
# build FROM it instead of cloning paxx12 at build time. Only the external dependencies are pulled:
# Rockchip MPP, libdatachannel (WebRTC), and live555 (RTSP), pinned to the commits the fork used.
FROM --platform=linux/arm64 debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential make cmake git pkg-config \
    ninja-build curl wget python3 ca-certificates libssl-dev libv4l-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build/v4l2-mpp

# Pulled deps + their build are an expensive (QEMU) layer that depends only on the dep pins and the
# compile scripts, never on our app code. COPY just those first so editing an app does not rebuild
# Rockchip MPP / libdatachannel / live555.
COPY src/v4l2-mpp/Makefile ./Makefile
COPY src/v4l2-mpp/deps ./deps

RUN git clone https://github.com/HermanChen/mpp.git deps/mpp \
    && git -C deps/mpp checkout 437bfbeb9567cca9cd9080e3f6954aa9d6a94f18

RUN git clone https://github.com/paullouisageneau/libdatachannel.git deps/libdatachannel \
    && git -C deps/libdatachannel checkout 222529eb2c8ae44f96462504ae38023f62809cec \
    && git -C deps/libdatachannel submodule update --init --recursive

RUN make deps

# Our owned app/common source + the local C shims. These layers rebuild on a code change; the deps
# layers above stay cached.
COPY src/v4l2-mpp/apps ./apps
COPY src/v4l2-mpp/common ./common
COPY src/fake-service /build/fake-service
COPY src/v4l2-imposter /build/v4l2-imposter

RUN make install DESTDIR=/dist \
    && make -C /build/fake-service install DESTDIR=/dist \
    && make -C /build/v4l2-imposter install DESTDIR=/dist

RUN mkdir -p /out/html \
    && find /dist/usr/local/bin -maxdepth 1 -type f ! -name "detect-*.py" -exec cp {} /out/ \; \
    && find /dist/usr/local/lib -maxdepth 1 -name "*.so" -exec cp {} /out/ \; \
    && cp -r /dist/usr/share/camera-stream/html/. /out/html/ 2>/dev/null || true

# Single stage on purpose: the whole toolchain (apt packages + the v4l2-mpp `make deps`) is one
# cacheable layer chain pinned to the v4l2-mpp commit, so a GitHub Actions layer cache restores it
# and only the `COPY src` + `make install` layers rebuild when our code changes. Targets
# linux/arm64 (Rockchip U1); emulated under QEMU on an x86 runner.
FROM --platform=linux/arm64 debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential make cmake git pkg-config \
    ninja-build curl wget python3 ca-certificates libssl-dev libv4l-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build/v4l2-mpp
RUN git clone --recursive https://github.com/paxx12/v4l2-mpp.git . \
    && git checkout 10fc3b9d935d9c79bacc014839c05de4a004c4ac \
    && git submodule update --init --recursive

RUN make deps

COPY src/fake-service /build/fake-service
COPY src/v4l2-imposter /build/v4l2-imposter

RUN make install DESTDIR=/dist \
    && make -C /build/fake-service install DESTDIR=/dist \
    && make -C /build/v4l2-imposter install DESTDIR=/dist

RUN mkdir -p /out/html \
    && find /dist/usr/local/bin -maxdepth 1 -type f ! -name "detect-*.py" -exec cp {} /out/ \; \
    && find /dist/usr/local/lib -maxdepth 1 -name "*.so" -exec cp {} /out/ \; \
    && cp -r /dist/usr/share/stream-http/html/. /out/html/ 2>/dev/null || true

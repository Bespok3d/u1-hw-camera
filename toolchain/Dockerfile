FROM bespok3d-build-base

RUN apt-get update && apt-get install -y --no-install-recommends \
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

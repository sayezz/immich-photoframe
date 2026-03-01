# =============================================================================
# Dockerfile — immich-photoframe
# Multi-stage build: compile in a full builder image, copy binary to a leaner
# runtime image.
#
# Requires an X11 display on the host (see docker-compose.yml).
# =============================================================================

# ---- Build stage ------------------------------------------------------------
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libopencv-dev \
        libqrencode-dev \
        libcurl4-openssl-dev \
        libx11-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt .
COPY main.cpp DisplayImage.cpp DisplayImage.h \
     ImmichClient.cpp ImmichClient.h json.hpp ./

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc)


# ---- Runtime stage ----------------------------------------------------------
FROM debian:bookworm-slim AS runtime

# Install only runtime shared libraries (no -dev headers).
# Package names on Debian 12 bookworm (arm64) use the "406" suffix.
# libopencv-videoio406 and the glib/font libs are needed by OpenCV highgui.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libopencv-core406 \
        libopencv-imgproc406 \
        libopencv-imgcodecs406 \
        libopencv-highgui406 \
        libopencv-videoio406 \
        libqrencode4 \
        libcurl4 \
        libx11-6 \
        libglib2.0-0 \
        libfontconfig1 \
        libfreetype6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/immich-photoframe ./immich-photoframe

# config.json and db.json are expected to be bind-mounted from the host
# (see docker-compose.yml) so the container always uses the host copies.
# Drop a template so the path exists even without a mount:
COPY config.json ./config.json.template

# Run as a non-root user for slightly better security
RUN useradd -m -u 1000 photoframe
USER photoframe

ENTRYPOINT ["./immich-photoframe"]

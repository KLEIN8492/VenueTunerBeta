# Linux x64 build environment for the VenueTuner engine (Ubuntu 22.04: glibc 2.35).
# X11 headers are only for JUCE's build helper (juceaide); the engine itself is headless.
#   docker build --platform linux/amd64 -t venuetuner-linux engine/platform/linux
#   docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src venuetuner-linux \
#     sh -c 'cmake -S engine -B engine/build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build engine/build-linux --target VenueTunerEngine vt_tests'
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++-12 ninja-build git ca-certificates pkg-config python3-pip \
      libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libcurl4-openssl-dev \
      libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxcomposite-dev \
    && pip3 install --no-cache-dir cmake==3.31.6 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-12 100 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-12 100 \
    && rm -rf /var/lib/apt/lists/*

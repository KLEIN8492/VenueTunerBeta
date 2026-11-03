# Experimental Windows x64 cross-build of the VenueTuner engine with MinGW-w64 (WASAPI; no ASIO).
# The supported Windows build is MSVC on a Windows PC: npm run package:win.
#   docker build --platform linux/amd64 -t venuetuner-mingw engine/platform/windows
#   docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src venuetuner-mingw sh -c \
#     'cmake -S engine -B engine/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#        -DCMAKE_TOOLCHAIN_FILE=/src/engine/platform/windows/mingw-w64.cmake && cmake --build engine/build-win'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++-mingw-w64-x86-64-posix g++ ninja-build cmake git ca-certificates pkg-config wine64 \
      libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libfreetype-dev libfontconfig1-dev \
    && rm -rf /var/lib/apt/lists/*

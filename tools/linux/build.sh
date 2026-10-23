#!/bin/sh
# Runs inside the kinst-linux container: repo at /src, build tree in /build (a Docker volume, fast),
# then the tests (OpenGL ones on Xvfb + Mesa) and a copy of the binary to /src/build-linux/.
set -e
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /build
cd /build
SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 1280x720x24" ctest --output-on-failure
mkdir -p /src/build-linux
cp /build/kinst /src/build-linux/kinst
echo "built /src/build-linux/kinst"

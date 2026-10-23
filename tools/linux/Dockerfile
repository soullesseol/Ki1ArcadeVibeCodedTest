# Linux build environment. Ubuntu 22.04 (glibc 2.35) keeps the binary runnable on older systems
# (SteamOS 3.5+, distros from 2022 on); gcc 13 comes from the toolchain PPA for std::format, and
# libstdc++/libgcc are linked statically (CMakeLists), so glibc is the only version requirement.
# SDL2 is built from source by CMake; these are its X11/Wayland/GL/audio headers (it loads the
# libraries at runtime), plus Xvfb + Mesa so the OpenGL renderer test runs without a display.
#   docker build -t kinst-linux tools/linux
#   docker run --rm -v "<repo>:/src" -v kinst-linux-build:/build kinst-linux
FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        software-properties-common gpg-agent \
    && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        gcc-13 g++-13 make cmake ninja-build git ca-certificates pkg-config \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev \
        libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
        xvfb xauth libgl1-mesa-dri \
    && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-13 CXX=g++-13
COPY build.sh /usr/local/bin/kinst-build
CMD ["kinst-build"]

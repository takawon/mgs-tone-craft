#!/usr/bin/env bash
# Idempotent Cloud Agent bootstrap for MGS Tone Craft.
#
# The full JUCE/WASAPI GUI app (target "mgstc") is Windows-only and is guarded
# by if(WIN32) in CMakeLists.txt, so it is not built on the Linux Cloud Agent.
# On Linux we build and validate the cross-platform scope: the emu2149/emu2212/
# emu2413 chip emulators, the mgstc_engine static library, the engine test suite
# (mgstc_engine_tests) and the chip-level diagnostic probe (mgstc_chip_level_probe).
set -euo pipefail

BUILD_DIR="build"

# System toolchain. The default image ships cmake/gcc/g++/git but is missing
# ninja and the libstdc++ development files; apt-get install is idempotent.
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    libstdc++-13-dev \
    ninja-build

# Pin the build to GCC: the default cc/c++ alternatives point at clang, which
# targets a gcc-14 toolchain whose libstdc++ dev symlink is absent in the image.
cmake -S . -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++

cmake --build "${BUILD_DIR}" --parallel

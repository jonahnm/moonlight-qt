#!/bin/bash
#
# Builds the PyroWave shared library (libpyrowave-shared) for macOS with MoltenVK.
#
# The PyroWave CMake build assumes GNU ld (--version-script); this script applies
# a small patch (pyrowave/macos.patch) that switches the dylib to an @rpath install
# name and an ld64-compatible -exported_symbols_list. The resulting dylib is linked
# by the app and bundled by macdeployqt; the Vulkan instance/device is shared with
# libplacebo via the MoltenVK ICD from libs/mac.
set -e

PYROWAVE_DIR=$(cd "$(dirname "$0")/../pyrowave" && pwd)
SCRIPTS_DIR=$(cd "$(dirname "$0")" && pwd)

echo "Checking out Granite dependencies"
# checkout_granite.sh resolves Granite relative to its own working directory.
(cd "$PYROWAVE_DIR" && ./checkout_granite.sh)

echo "Applying macOS build patches"
cd "$PYROWAVE_DIR"
if git apply --check --reverse "$SCRIPTS_DIR/pyrowave-macos.patch" 2>/dev/null; then
    echo "pyrowave-macos.patch already applied"
else
    git apply "$SCRIPTS_DIR/pyrowave-macos.patch"
fi
cd "$PYROWAVE_DIR/Granite"
if git apply --check --reverse "$SCRIPTS_DIR/pyrowave-macos-granite.patch" 2>/dev/null; then
    echo "pyrowave-macos-granite.patch already applied"
else
    git apply "$SCRIPTS_DIR/pyrowave-macos-granite.patch"
fi

echo "Configuring PyroWave build"
cd "$PYROWAVE_DIR"
# Match the app's deployment target (see app.pro / generate-dmg.sh) so the dylib
# loads on older macOS versions.
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0

echo "Building PyroWave"
cmake --build build -j

echo "PyroWave build complete: $PYROWAVE_DIR/build/libpyrowave-shared.dylib"

#!/bin/bash
set -e

# Setup paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLYCAST_DIR="$SCRIPT_DIR"
BUILD_DIR="$FLYCAST_DIR/build_GBAStation"
export TMPDIR="${SCRIPT_DIR}/.codex_tmp/msys_tmp"
export TMP="${TMPDIR}"
export TEMP="${TMPDIR}"
mkdir -p "${TMPDIR}"

# NVK (Mesa Vulkan driver for Switch). Prefer the sibling packaged SDK that
# switchVK produces, but keep /nvk-build for Docker-style environments.
if [ -z "${MESA_NVK_DIR:-}" ]; then
    for candidate in \
        "$SCRIPT_DIR/../switchVK/nvk-switch-26.1.4" \
        "$SCRIPT_DIR/../switchVK/nvk-switch-25.3.6" \
        "/nvk-build"; do
        if [ -d "$candidate" ]; then
            MESA_NVK_DIR="$candidate"
            break
        fi
    done
fi
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"

echo "=== Building GBAStation Flycast Stub (Vulkan) ==="
echo "Source: $FLYCAST_DIR"
echo "Build:  $BUILD_DIR"
echo "NVK:    $MESA_NVK_DIR"

if [ ! -d "$FLYCAST_DIR" ]; then
    echo "Error: Flycast source directory not found at $FLYCAST_DIR"
    exit 1
fi

CMAKE_GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    CMAKE_GENERATOR_ARGS=(-G Ninja)
    if [ -f "$BUILD_DIR/CMakeCache.txt" ] && ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$BUILD_DIR/CMakeCache.txt"; then
        echo "Existing build directory uses a different CMake generator; recreating it for Ninja."
        rm -rf "$BUILD_DIR"
    fi
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure for Switch with the GBAStation Vulkan frontend.
# LIBRETRO=ON enables libretro core building, USE_GBAStation=ON wires in the
# GBAStation-flycast.nro target. USE_VULKAN is forced ON inside CMakeLists.txt
# for this combination — no need to pass it here.
cmake "${CMAKE_GENERATOR_ARGS[@]}" "$FLYCAST_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DPLATFORM=libnx \
    -DLIBRETRO=ON \
    -DUSE_GBAStation=ON \
    -DNINTENDO_SWITCH=ON \
    -DARCHITECTURE=arm64 \
    -DMESA_NVK_DIR="$MESA_NVK_DIR" \
    -DDISABLE_LOGGING=OFF \
    -DCMAKE_BUILD_TYPE=Release

# Build
echo "Running build..."
cmake --build . -j"${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"

echo ""
echo "=== Build Complete ==="

# Check for NRO output (might be named based on output_name or target name)
NRO_FILE=""
if [ -f "GBAStationFlycastStub.nro" ]; then
    NRO_FILE="GBAStationFlycastStub.nro"
elif [ -f "GBAStation-flycast.nro" ]; then
    NRO_FILE="GBAStation-flycast.nro"
elif [ -f "flycast_libretro.nro" ]; then
    NRO_FILE="flycast_libretro.nro"
fi

if [ -n "$NRO_FILE" ]; then
    echo "SUCCESS: $NRO_FILE generated."
    echo "Path: $BUILD_DIR/$NRO_FILE"
    
    # Copy to top level for easy access
    cp "$NRO_FILE" "$SCRIPT_DIR/GBAStationFlycastStub.nro"
    echo "Copied to: $SCRIPT_DIR/GBAStationFlycastStub.nro"
else
    echo "ERROR: No .nro file was created."
    echo "Checking for other outputs..."
    ls -la *.nro 2>/dev/null || echo "No .nro files found"
    ls -la *.elf 2>/dev/null || echo "No .elf files found"
    exit 1
fi

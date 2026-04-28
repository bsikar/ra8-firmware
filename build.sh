#!/bin/bash
# Portable CMake build script for ra8d2-firmware
# Works on: WSL, Linux, macOS, Dev Containers

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== ra8d2-firmware - CMake Build ===${NC}"

# Check for arm-none-eabi-gcc and auto-add common locations to PATH
if ! command -v arm-none-eabi-gcc &> /dev/null; then
    # Try common ARM GNU Toolchain installation locations
    COMMON_PATHS=(
        "$HOME/arm-gnu-toolchain/bin"
        "$HOME/gcc-arm-none-eabi/bin"
        "/opt/arm-gnu-toolchain/bin"
        "/usr/local/arm-gnu-toolchain/bin"
    )

    for ARM_PATH in "${COMMON_PATHS[@]}"; do
        if [ -f "$ARM_PATH/arm-none-eabi-gcc" ]; then
            export PATH="$ARM_PATH:$PATH"
            echo -e "${YELLOW}Added $ARM_PATH to PATH${NC}"
            break
        fi
    done

    # Check again after trying to add to PATH
    if ! command -v arm-none-eabi-gcc &> /dev/null; then
        echo -e "${RED}Error: arm-none-eabi-gcc not found in PATH${NC}"
        echo "Install the ARM GNU Toolchain from:"
        echo "  https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
        echo "then add it to PATH, e.g.:"
        echo "  export PATH=\"\$HOME/arm-gnu-toolchain/bin:\$PATH\""
        exit 1
    fi
fi

echo -e "${GREEN}[PASS] ARM GNU Toolchain found:${NC} $(arm-none-eabi-gcc --version | head -1)"

# Parse arguments
BUILD_TYPE="Debug"
CLEAN=false
VERBOSE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        clean)
            CLEAN=true
            shift
            ;;
        release|Release)
            BUILD_TYPE="Release"
            shift
            ;;
        debug|Debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        relwithdebinfo|RelWithDebInfo)
            BUILD_TYPE="RelWithDebInfo"
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  clean             Clean build directory before building"
            echo "  debug             Build in Debug mode (default)"
            echo "  release           Build in Release mode (-Os)"
            echo "  relwithdebinfo    Build in RelWithDebInfo mode (-Og)"
            echo "  -v, --verbose     Verbose build output"
            echo "  -h, --help        Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            echo "Use -h for help"
            exit 1
            ;;
    esac
done

# Clean if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"
    echo -e "${GREEN}[PASS] Clean complete${NC}"
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Configure with CMake
echo -e "${YELLOW}Configuring CMake (${BUILD_TYPE} mode)...${NC}"
cd "$BUILD_DIR"

# EXAMPLE env var picks the application entry point (subdir of examples/).
# Defaults to "blink" via the top-level CMakeLists.txt.
example_arg=()
if [[ -n "${EXAMPLE:-}" ]]; then
    example_arg+=(-DEXAMPLE="${EXAMPLE}")
fi

cmake \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/cmake/toolchain-ra8d2.cmake" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "${example_arg[@]}" \
    ..

echo -e "${GREEN}[PASS] Configuration complete${NC}"

# Build
echo -e "${YELLOW}Building firmware...${NC}"

# Portable CPU count (nproc on Linux, sysctl on macOS)
if command -v nproc &> /dev/null; then
    JOBS="$(nproc)"
else
    JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 1)"
fi

if [ "$VERBOSE" = true ]; then
    cmake --build . --verbose
else
    cmake --build . -- -j"${JOBS}"
fi

echo ""
echo -e "${GREEN}[PASS] Build successful!${NC}"
echo ""
echo -e "${GREEN}Build artifacts:${NC}"
find . -maxdepth 1 \( -name '*.elf' -o -name '*.hex' -o -name '*.bin' -o -name '*.map' \) \
     -exec ls -lh {} + 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'
echo ""
echo -e "${GREEN}ELF:${NC}   ra8d2-firmware.elf"
echo -e "${GREEN}HEX:${NC}   ra8d2-firmware.hex"
echo -e "${GREEN}BIN:${NC}   ra8d2-firmware.bin"

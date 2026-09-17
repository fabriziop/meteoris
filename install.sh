#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAST_BUILD_FILE="${ROOT_DIR}/.meteoris-last-build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
MODE=""
PREFIX=""
BUILD_DIR=""
INSTALL_SIM=1
REBUILD=0

usage() {
    cat <<USAGE
Usage: ./install.sh --local | --system [options] [cmake-options...]

  --local           Install for the current user (default prefix: \$HOME/.local).
  --system          Install system-wide (default prefix: /usr/local).
  --prefix DIR      Override the installation prefix.
  --build-dir DIR   Install from this configured build directory.
  --rebuild         Build again before installing.
  --no-sim          Do not install the SoapySDR simulator plugin.

Normally install.sh reuses the last successful ./build.sh directory and does
not configure or compile again.

Examples:
  ./build.sh
  sudo ./install.sh --system
  ./install.sh --local --build-dir build-recorder-only
  sudo ./install.sh --system --rebuild
USAGE
}

CMAKE_ARGS=()
while (($#)); do
    case "$1" in
        --local)
            [[ -z "$MODE" ]] || { echo "Choose only one of --local/--system" >&2; exit 2; }
            MODE="local"
            ;;
        --system)
            [[ -z "$MODE" ]] || { echo "Choose only one of --local/--system" >&2; exit 2; }
            MODE="system"
            ;;
        --prefix)
            shift
            [[ $# -gt 0 ]] || { echo "--prefix requires a directory" >&2; exit 2; }
            PREFIX="$1"
            ;;
        --build-dir)
            shift
            [[ $# -gt 0 ]] || { echo "--build-dir requires a directory" >&2; exit 2; }
            BUILD_DIR="$1"
            ;;
        --rebuild)
            REBUILD=1
            ;;
        --no-sim)
            INSTALL_SIM=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            CMAKE_ARGS+=("$1")
            ;;
    esac
    shift
done

[[ -n "$MODE" ]] || { usage >&2; exit 2; }

if [[ -z "$PREFIX" ]]; then
    if [[ "$MODE" == "local" ]]; then
        PREFIX="${HOME}/.local"
    else
        PREFIX="/usr/local"
    fi
fi

if [[ -n "$BUILD_DIR" ]]; then
    if [[ "$BUILD_DIR" != /* ]]; then
        BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
    fi
elif [[ -f "$LAST_BUILD_FILE" ]]; then
    BUILD_DIR="$(head -n 1 "$LAST_BUILD_FILE")"
    echo "Reusing last successful build: ${BUILD_DIR}"
else
    echo "No previous build recorded; creating a full native build first."
    "${ROOT_DIR}/build.sh" --full --native
    BUILD_DIR="$(head -n 1 "$LAST_BUILD_FILE")"
fi

[[ -f "${BUILD_DIR}/CMakeCache.txt" ]] || {
    echo "Build directory is not configured: ${BUILD_DIR}" >&2
    echo "Run ./build.sh first, or use --build-dir DIR." >&2
    exit 2
}

if [[ ${#CMAKE_ARGS[@]} -gt 0 ]]; then
    echo "Applying CMake options to existing build: ${BUILD_DIR}"
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${CMAKE_ARGS[@]}"
    REBUILD=1
fi

if [[ "$REBUILD" -eq 1 ]]; then
    echo "Building before install: ${BUILD_DIR}"
    cmake --build "$BUILD_DIR" --parallel "$(nproc)"
else
    echo "Installing existing build without rebuilding: ${BUILD_DIR}"
fi

if [[ "$INSTALL_SIM" -eq 1 ]] && grep -q '^METEORIS_BUILD_SIM:BOOL=ON$' "${BUILD_DIR}/CMakeCache.txt"; then
    cmake --install "$BUILD_DIR" --prefix "$PREFIX"
else
    cmake --install "$BUILD_DIR" --component meteoris_runtime --prefix "$PREFIX"
fi

echo
echo "Installed from: ${BUILD_DIR}"
echo "Install prefix: ${PREFIX}"
echo "  ${PREFIX}/bin/meteoris"
if grep -q '^METEORIS_INSTALL_PLOT:BOOL=ON$' "${BUILD_DIR}/CMakeCache.txt"; then
    echo "  ${PREFIX}/bin/meteoris_plot"
fi
echo "  ${PREFIX}/bin/meteoris_config"
echo "  ${PREFIX}/bin/meteoris_recover_hdf5"
echo "  ${PREFIX}/share/meteoris/"

if [[ "$MODE" == "local" && ":${PATH}:" != *":${PREFIX}/bin:"* ]]; then
    echo
    echo "NOTE: ${PREFIX}/bin is not currently in PATH."
    echo "Add this to your shell startup file:"
    echo "  export PATH=\"${PREFIX}/bin:\$PATH\""
fi

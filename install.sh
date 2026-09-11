#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
MODE=""
PREFIX=""
INSTALL_SIM=1

usage() {
    cat <<USAGE
Usage: ./install.sh --local | --system [--prefix DIR] [--no-sim] [cmake-options...]

  --local       Install Meteoris, meteoris_plot, meteoris_recover_hdf5 and
                the meteoris_sim plugin for the current user.
                Default prefix: $HOME/.local

  --system      Install system-wide, including meteoris_recover_hdf5 and
                the meteoris_sim plugin.
                Default prefix: /usr/local
                Run this mode with sudo if the prefix is not writable.

  --prefix DIR  Override the installation prefix.

  --no-sim      Do not build/install the SoapySDR simulator plugin.

Examples:
  ./install.sh --local
  sudo ./install.sh --system
  ./install.sh --local -DMETEORIS_NATIVE_OPTIMIZATION=OFF
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

if [[ "$INSTALL_SIM" -eq 0 ]]; then
    CMAKE_ARGS+=("-DMETEORIS_BUILD_SIM=OFF")
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    "${CMAKE_ARGS[@]}"

cmake --build "$BUILD_DIR" --parallel "$(nproc)"

if [[ "$INSTALL_SIM" -eq 1 ]]; then
    # Install all normal runtime targets AND the freshly built Soapy module.
    # This is important: leaving an older meteoris_sim module installed makes
    # simulator source changes appear to have no effect.
    cmake --install "$BUILD_DIR" --prefix "$PREFIX"
else
    cmake --install "$BUILD_DIR" \
        --component meteoris_runtime \
        --prefix "$PREFIX"
fi

echo
echo "Installed:"
echo "  $PREFIX/bin/meteoris"
echo "  $PREFIX/bin/meteoris_plot"
echo "  $PREFIX/bin/meteoris_recover_hdf5"
echo "  $PREFIX/share/meteoris/"
if [[ "$INSTALL_SIM" -eq 1 ]]; then
    echo "  SoapySDR meteoris_sim plugin (CMake Soapy module directory)"
fi

if [[ "$MODE" == "local" && ":${PATH}:" != *":${PREFIX}/bin:"* ]]; then
    echo
    echo "NOTE: ${PREFIX}/bin is not currently in PATH."
    echo "Add this to your shell startup file:"
    echo "  export PATH=\"${PREFIX}/bin:\$PATH\""
fi

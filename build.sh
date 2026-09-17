#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
LAST_BUILD_FILE="${ROOT_DIR}/.meteoris-last-build"

usage() {
    cat <<'USAGE'
Usage: ./build.sh [--full | --recorder-only] [--native | --cross] [cmake-options...]

Build mode:
  --full           Build recorder + plot + config wizard + HDF5 recovery tool.
  --recorder-only  Build recorder + config wizard + HDF5 recovery tool.

Target mode:
  --native         Build for the current machine.
  --cross          Cross-build for aarch64 using RPI_SYSROOT.

If no build mode is supplied, an interactive 1/2 menu is shown.
Target mode is normally automatic: native on the current machine, or cross
when RPI_SYSROOT is set on a non-Raspberry-Pi host.

Examples:
  ./build.sh
  ./build.sh --full
  ./build.sh --recorder-only
  ./build.sh --full -DMETEORIS_USE_FFTW=OFF
  RPI_SYSROOT=/opt/sysroots/rpi-aarch64 ./build.sh --cross --recorder-only
USAGE
}

is_raspberry_pi_native() {
    if [[ -r /proc/device-tree/model ]] && grep -qi "raspberry pi" /proc/device-tree/model; then
        return 0
    fi
    if [[ -r /etc/os-release ]] && grep -qi "raspberry pi os\|raspbian" /etc/os-release; then
        return 0
    fi
    return 1
}

is_raspberry_pi_3() {
    [[ -r /proc/device-tree/model ]] && grep -qi "raspberry pi 3" /proc/device-tree/model
}

build_mode=""
target_mode=""
cmake_args=()

while (($#)); do
    case "$1" in
        --full)
            [[ -z "${build_mode}" ]] || { echo "Choose only one of --full/--recorder-only" >&2; exit 2; }
            build_mode="full"
            ;;
        --recorder-only)
            [[ -z "${build_mode}" ]] || { echo "Choose only one of --full/--recorder-only" >&2; exit 2; }
            build_mode="recorder-only"
            ;;
        --native)
            [[ -z "${target_mode}" ]] || { echo "Choose only one of --native/--cross" >&2; exit 2; }
            target_mode="native"
            ;;
        --cross)
            [[ -z "${target_mode}" ]] || { echo "Choose only one of --native/--cross" >&2; exit 2; }
            target_mode="cross"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            cmake_args+=("$1")
            ;;
    esac
    shift
done

if [[ -z "${build_mode}" ]]; then
    echo "Build mode:"
    echo "  1) Full"
    echo "     meteoris + meteoris_plot + meteoris_config + meteoris_recover_hdf5"
    echo "  2) Recorder only"
    echo "     meteoris + meteoris_config + meteoris_recover_hdf5"
    read -r -p "Choice [1]: " choice
    case "${choice:-1}" in
        1) build_mode="full" ;;
        2) build_mode="recorder-only" ;;
        *) echo "Invalid choice: ${choice}" >&2; exit 2 ;;
    esac
fi

if [[ -z "${target_mode}" ]]; then
    if is_raspberry_pi_native; then
        target_mode="native"
    elif [[ -n "${RPI_SYSROOT:-}" ]]; then
        target_mode="cross"
    else
        target_mode="native"
    fi
fi

if [[ "${target_mode}" == "cross" && -z "${RPI_SYSROOT:-}" ]]; then
    echo "--cross requires RPI_SYSROOT to be set." >&2
    echo "Example: export RPI_SYSROOT=/opt/sysroots/rpi-aarch64" >&2
    exit 2
fi

if [[ "${target_mode}" == "cross" ]]; then
    BUILD_DIR="${ROOT_DIR}/build-cross-${build_mode}"
else
    BUILD_DIR="${ROOT_DIR}/build-${build_mode}"
fi

configure_args=(
    -S "${ROOT_DIR}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DMETEORIS_INSTALL_RECOVER_HDF5=ON
    -DMETEORIS_INSTALL_CONFIG_TOOL=ON
)

if [[ "${build_mode}" == "full" ]]; then
    configure_args+=(
        -DMETEORIS_INSTALL_PLOT=ON
        -DMETEORIS_BUILD_SIM=ON
    )
else
    configure_args+=(
        -DMETEORIS_INSTALL_PLOT=OFF
        -DMETEORIS_BUILD_SIM=OFF
    )
fi

if [[ "${target_mode}" == "cross" ]]; then
    configure_args+=(
        -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchains/aarch64-linux.cmake"
        -DRPI_SYSROOT="${RPI_SYSROOT}"
        -DMETEORIS_NATIVE_OPTIMIZATION=OFF
        -DMETEORIS_CPU_TUNE=cortex-a53
        -DMETEORIS_BUILD_SIM=OFF
    )
elif is_raspberry_pi_3; then
    # Preserve the previous Pi 3 tuning while keeping generic build filenames.
    configure_args+=(-DMETEORIS_CPU_TUNE=cortex-a53)
fi

configure_args+=("${cmake_args[@]}")

echo "Build mode:  ${build_mode}"
echo "Target mode: ${target_mode}"
echo "Build dir:   ${BUILD_DIR}"

cmake "${configure_args[@]}"
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

printf '%s\n' "${BUILD_DIR}" > "${LAST_BUILD_FILE}"
echo
echo "Build complete. install.sh will reuse: ${BUILD_DIR}"

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

usage() {
        cat <<USAGE
Usage: ./build_rpi3.sh [--native | --cross] [cmake-options...]

    --native   Force native Raspberry Pi build preset.
    --cross    Force cross-build preset (requires RPI_SYSROOT).

Without overrides:
    - On Raspberry Pi hardware/OS: native preset is selected.
    - Else if RPI_SYSROOT is set: cross preset is selected.
USAGE
}

is_raspberry_pi_native() {
    # Hardware-first check (most reliable on Linux).
    if [[ -r /proc/device-tree/model ]] && grep -qi "raspberry pi" /proc/device-tree/model; then
        return 0
    fi

    # Fallback check for Raspberry Pi OS userland.
    if [[ -r /etc/os-release ]] && grep -qi "raspberry pi os\|raspbian" /etc/os-release; then
        return 0
    fi

    return 1
}

mode_override=""
cmake_args=()
while (($#)); do
    case "$1" in
        --native)
            [[ -z "${mode_override}" ]] || { echo "Choose only one of --native/--cross" >&2; exit 2; }
            mode_override="native"
            ;;
        --cross)
            [[ -z "${mode_override}" ]] || { echo "Choose only one of --native/--cross" >&2; exit 2; }
            mode_override="cross"
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

echo "Select Raspberry Pi build mode:"
echo "  1) Full build (meteoris + meteoris_plot + meteoris_recover_hdf5)"
echo "  2) Recorder-only build (meteoris + meteoris_recover_hdf5)"
read -r -p "Choice [1/2]: " choice

case "${choice}" in
    1)
        native_preset="rpi3-native-release-full"
        cross_preset="rpi3-aarch64-cross-release-full"
        ;;
    2)
        native_preset="rpi3-native-release-recorder-only"
        cross_preset="rpi3-aarch64-cross-release-recorder-only"
        ;;
    *)
        echo "Invalid choice: ${choice}" >&2
        exit 2
        ;;
esac

if [[ "${mode_override}" == "native" ]]; then
    preset="${native_preset}"
    echo "Using native build preset: ${preset}"
elif [[ "${mode_override}" == "cross" ]]; then
    [[ -n "${RPI_SYSROOT:-}" ]] || {
        echo "--cross requires RPI_SYSROOT to be set." >&2
        echo "Example: export RPI_SYSROOT=/opt/sysroots/rpi-trixie-aarch64" >&2
        exit 2
    }
    preset="${cross_preset}"
    echo "Using cross build preset: ${preset}"
elif is_raspberry_pi_native; then
    preset="${native_preset}"
    echo "Detected Raspberry Pi environment; using native build preset: ${preset}"
elif [[ -n "${RPI_SYSROOT:-}" ]]; then
    preset="${cross_preset}"
    echo "Using cross build preset: ${preset}"
else
    echo "Cannot determine Raspberry Pi native environment and RPI_SYSROOT is not set." >&2
    echo "Set RPI_SYSROOT to cross-compile from this host, or run this script on the Raspberry Pi." >&2
    exit 2
fi

cmake --preset "${preset}" "${cmake_args[@]}"
cmake --build --preset "${preset}"

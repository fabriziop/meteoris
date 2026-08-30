#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
MODULE_DIR="${BUILD_DIR}/soapy-modules"
CONFIG_FILE="${ROOT_DIR}/config/meteoris_sim.toml"

if [[ ! -x "${BUILD_DIR}/meteoris" ]] || ! compgen -G "${MODULE_DIR}/*meteorisSim*" >/dev/null; then
    "${ROOT_DIR}/build.sh"
fi

# Prefer this repository's freshly built simulator module. This avoids silently
# running an older system/user-installed meteoris_sim plugin during development.
export SOAPY_SDR_PLUGIN_PATH="${MODULE_DIR}"

exec "${BUILD_DIR}/meteoris" --config "${CONFIG_FILE}" "$@"

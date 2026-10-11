#!/usr/bin/env bash
# Usage: ./GenerateProjectFiles.sh [gmake|ninja]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
action="${1:-gmake}"
premake_dir="${root}/External/Premake/Linux"
premake="${premake_dir}/premake5"

if [[ ! -x "${premake}" ]]; then
    archive="$(mktemp --suffix=.tar.gz)"
    curl -fsSL -o "${archive}" "https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-linux.tar.gz"
    echo "63edd3e7461eebdd45b500a3c7e8ad4e7a67d68f230010f9a97cbb71b4ec59c8  ${archive}" | sha256sum -c --quiet
    mkdir -p "${premake_dir}"
    tar -xzf "${archive}" -C "${premake_dir}"
    rm -f "${archive}"
fi

"${premake}" --file="${root}/premake5.lua" "${action}"

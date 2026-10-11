#!/usr/bin/env bash
# Builds the Linux port and runs the archive checks inside WSL.
# Usage from Windows: wsl -d archlinux -- bash Scripts/WslCheck.sh [gcc|clang]
# The tree is copied to the Linux filesystem first: builds on /mnt/c are slow, and gmake output would land in the Windows checkout.
set -euo pipefail

compiler="${1:-gcc}"
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${HOME}/OpenVyruz-wsl/${compiler}"
data_root="$(cd "${source_root}/../Vyruz/Game/Vyruz" && pwd)"

mkdir -p "${build_root}"
rsync -a --delete --exclude=/.git --exclude=/Binaries --exclude=/Intermediate --exclude=/External/Premake "${source_root}/" "${build_root}/"

cd "${build_root}"
./GenerateProjectFiles.sh gmake "--cc=${compiler}"
make --jobs="$(nproc)" config=development Vyruz

binary="${build_root}/Binaries/linux/Development/Vyruz"
"${binary}" --verify-gaf "${data_root}" | tail -n 1
"${binary}" --verify-levels "${data_root}" | tail -n 1

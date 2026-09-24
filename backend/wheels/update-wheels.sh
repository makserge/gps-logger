#!/usr/bin/env bash
# Regenerate the vendored wheels/ directory after changing
# backend/app/requirements.txt. Run this on a dev machine with internet
# access - the build itself (Docker / HA Supervisor) does not have it.
set -euo pipefail
cd "$(dirname "$0")"

rm -f ./*.whl

pip download -r ../app/requirements.txt -d . --only-binary=:all: \
  --platform musllinux_1_2_aarch64 --platform musllinux_1_1_aarch64 \
  --platform linux_aarch64 --python-version 312 --implementation cp \
  --abi cp312 --abi none --abi abi3

echo "wheels/ regenerated for aarch64. Add --platform musllinux_1_2_x86_64 \\"
echo "etc. too if you need to build this image on/for amd64."

#!/usr/bin/env bash
# Builds fliks.nro inside the devkitPro container. Nothing is installed on the host.
set -euo pipefail
cd "$(dirname "$0")"
IMAGE=${DEVKITPRO_IMAGE:-devkitpro/devkita64:latest}
exec docker run --rm -t \
  -v "$PWD":/work -w /work \
  -u "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  "$IMAGE" bash -lc "make ${*:-}"

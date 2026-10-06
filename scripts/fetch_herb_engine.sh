#!/usr/bin/env bash
set -euo pipefail

REPO_BASE_URL="https://raw.githubusercontent.com/42henry/herb-engine-C-3D/main"
OUT_DIR="third_party/herb-engine-C-3D"

mkdir -p "${OUT_DIR}"

echo "Fetching herb-engine external sources into ${OUT_DIR}..."

curl -fsSL "${REPO_BASE_URL}/README.md" -o "${OUT_DIR}/README.md"
curl -fsSL "${REPO_BASE_URL}/herbengineC3D.c" -o "${OUT_DIR}/herbengineC3D.c"
curl -fsSL "${REPO_BASE_URL}/linux-platform.c" -o "${OUT_DIR}/linux-platform.c"
curl -fsSL "${REPO_BASE_URL}/windows-platform.c" -o "${OUT_DIR}/windows-platform.c"
curl -fsSL "${REPO_BASE_URL}/web-platform.c" -o "${OUT_DIR}/web-platform.c"

echo "Done."
echo "Fetched files:"
echo " - ${OUT_DIR}/README.md"
echo " - ${OUT_DIR}/herbengineC3D.c"
echo " - ${OUT_DIR}/linux-platform.c"
echo " - ${OUT_DIR}/windows-platform.c"
echo " - ${OUT_DIR}/web-platform.c"

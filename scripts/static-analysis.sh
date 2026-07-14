#!/usr/bin/env bash
# Static analysis of the application sources with cppcheck.
# Vendor code (HAL, TinyUSB, CubeMX-generated) is intentionally excluded —
# only App/Src is our responsibility to keep clean.
#
# Usage: scripts/static-analysis.sh
# Exits non-zero if cppcheck reports any finding.

set -euo pipefail
cd "$(dirname "$0")/.."

cppcheck \
    --std=c11 \
    --language=c \
    --platform=unix32 \
    --enable=warning,performance,portability \
    --inline-suppr \
    --suppressions-list=.cppcheck-suppressions \
    --error-exitcode=1 \
    --quiet \
    -I App/Inc \
    -DSTM32H503xx \
    -DUSE_HAL_DRIVER \
    App/Src

echo "static analysis: OK"

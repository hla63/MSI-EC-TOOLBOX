#!/bin/bash
# ---------------------------------------------------------------------------
# build.sh — Compile MSIECToolboxDump CLI tool
# Requires: kext MSIECToolbox loaded; run from the logged-in console session (no sudo needed)
# Usage:    bash build.sh
#           ./MSIECToolboxDump [--offset 0xXX] [--watch] [--diff] [--json]
# ---------------------------------------------------------------------------
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== MSIECToolboxDump — Build ==="
echo "→ Compiling..."

swiftc "$SCRIPT_DIR/MSIECToolboxDump.swift" \
    -o "$SCRIPT_DIR/MSIECToolboxDump" \
    -framework Foundation \
    -framework IOKit \
    -O

echo "✅ Build OK → $SCRIPT_DIR/MSIECToolboxDump"
echo "   Run with: ./MSIECToolboxDump"

#!/bin/bash
# ---------------------------------------------------------------------------
# build.sh — Compile MSIECToolboxDump CLI tool
# Requires: kext MSIECToolbox loaded (sudo to run the output binary)
# Usage:    bash build.sh
#           sudo ./MSIECToolboxDump [--offset 0xXX] [--watch] [--diff] [--json]
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
echo "   Run with: sudo ./MSIECToolboxDump"

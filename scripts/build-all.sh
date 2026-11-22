#!/usr/bin/env bash
# Fetch the VST2 headers, build the JUCE ARM64 desktop VST2 and the MPC package.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

"$ROOT/scripts/fetch-vst2-sdk.sh"
"$ROOT/scripts/build-juce-aarch64.sh"
"$ROOT/scripts/build-mpc.sh"

echo
echo "Artifacts:"
echo "  dist/juce-linux-aarch64/QDelay-vst2-aarch64.so"
echo "  dist/mpc/Tilr - VST - QDelay/"
echo "  dist/Tilr - VST - QDelay-1.3.1.zip"

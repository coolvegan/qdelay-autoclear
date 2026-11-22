#!/usr/bin/env bash
# Fetch the VST 2.4 headers required by JUCE's VST2 wrapper.
#
# Steinberg no longer distributes the VST2 SDK and it is not redistributable,
# so the headers are not committed to this repository. They are only needed
# for the JUCE desktop VST2 build; the MPC build uses the clean-room ABI in
# mpc/framework/mpc_vst2_abi.h and does not need them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/third_party/vst2sdk/pluginterfaces/vst2.x"
BASE="${VST2_MIRROR:-https://raw.githubusercontent.com/R-Tur/VST_SDK_2.4/master/pluginterfaces/vst2.x}"

mkdir -p "$DEST"
for f in aeffect.h aeffectx.h; do
    if [ ! -f "$DEST/$f" ]; then
        echo "fetching $f"
        curl -fsSL -o "$DEST/$f" "$BASE/$f"
    else
        echo "already present: $DEST/$f"
    fi
done

echo "VST2 SDK headers ready in $DEST"

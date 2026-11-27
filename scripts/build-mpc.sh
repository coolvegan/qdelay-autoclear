#!/usr/bin/env bash
# Build the MPC VST2 (AArch64 + ARM32) and package the on-device plugin folder.
#
# Requires: docker, g++ (for the x86 host simulator test), python3.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MPC="$ROOT/mpc"
BUILD="$MPC/build"
PKG="$ROOT/dist/mpc/Tilr - VST - QDelay"
IMAGE="qdelay-mpc-cross"
CXX="${CXX:-g++}"

command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 1; }
command -v "$CXX" >/dev/null || { echo "$CXX is required for the host simulator" >&2; exit 1; }

echo "== 1/6  host build + Auto Clear strategy test + MPC host simulator test"
mkdir -p "$BUILD/native"
"$CXX" -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden \
    -I "$MPC/framework" -I "$ROOT/src" -Wl,--version-script="$MPC/tools/exports.map" \
    -o "$BUILD/native/qdelay.so" "$MPC/src/QDelayMpc.cpp"

"$CXX" -std=c++17 -O2 -I "$ROOT/src" -o "$BUILD/autoclear-test" "$MPC/tools/autoclear-test.cpp"
"$BUILD/autoclear-test"
"$BUILD/host_sim" "$BUILD/native/qdelay.so" test --seconds 3 --wav "$BUILD/qdelay-test.wav"
"$BUILD/host_sim" "$BUILD/native/qdelay.so" dump > "$MPC/params.json"

echo "== 2/6  generate the MPC touch UI skin"
python3 "$MPC/tools/gen_ui.py" \
    --params "$MPC/params.json" \
    --ui "$MPC/ui.json" \
    --assets-dir "$MPC/assets" \
    --out "$BUILD/Plugin Skins" \
    --kit-cache "$BUILD"

echo "== 3/6  build the cross-compiler image"
docker build -q -t "$IMAGE" -f docker/Dockerfile.mpc-cross .

echo "== 4/6  cross-compile arm64 and arm32"
mkdir -p "$BUILD/cross/arm64" "$BUILD/cross/arm32"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$ROOT":/work/src -w /work/src "$IMAGE" bash -lc '
set -euo pipefail
CFLAGS="-std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden \
 -ffunction-sections -fdata-sections -Wall -Wextra -I mpc/framework -I src"
LDFLAGS="-Wl,--gc-sections -Wl,-s -Wl,--version-script=mpc/tools/exports.map -static-libstdc++ -static-libgcc -Wl,--as-needed"

aarch64-linux-gnu-g++ $CFLAGS -march=armv8-a $LDFLAGS \
  -o mpc/build/cross/arm64/qdelay-arm64.so mpc/src/QDelayMpc.cpp

arm-linux-gnueabihf-g++ $CFLAGS -mcpu=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard $LDFLAGS \
  -o mpc/build/cross/arm32/qdelay-arm32.so mpc/src/QDelayMpc.cpp
'

echo "== 5/6  package the MPC folder"
rm -rf "$PKG"
mkdir -p "$PKG/VST" "$PKG/Plugin Skins"
cp "$MPC/plugin.json" "$MPC/version.xml" "$PKG/"
cp "$BUILD/cross/arm64/qdelay-arm64.so" "$PKG/VST/"
cp "$BUILD/cross/arm32/qdelay-arm32.so" "$PKG/VST/"
cp "$BUILD/Plugin Skins/"* "$PKG/Plugin Skins/"
cp "$MPC/assets/qdelay-bg.png" "$PKG/Plugin Skins/"

cat > "$PKG/INSTALL.txt" <<'EOF'
QDelay for Akai MPC - installation
==================================

1. Copy the whole folder

       Tilr - VST - QDelay

   into a folder named exactly

       VST2 Plugins

   at the root of a USB drive, SD card or the MPC's internal drive.

   The result must look like:

       <drive>/VST2 Plugins/Tilr - VST - QDelay/plugin.json
       <drive>/VST2 Plugins/Tilr - VST - QDelay/VST/qdelay-arm64.so
       <drive>/VST2 Plugins/Tilr - VST - QDelay/Plugin Skins/TUI.json

2. On the MPC enable:
       Preferences -> Enhancements (Beta) -> Enable Enhancements
       Preferences -> Enhancements (Beta) -> External Plugin Support

3. Rescan / restart the MPC. QDelay appears under External Plugins and can be
   added as an insert effect.

The arm64 build is for 64-bit MPC OS devices (MPC Live III / XL and later).
The arm32 build is for Gen1 devices (MPC Live / Live II / One / One+ / X /
Key 61 / Key 37 / Force) running MPC OS 3.10.1 or later.

QDelay is an insert effect. Its UI is the MPC touchscreen skin in
"Plugin Skins"; the 16 Q-Link knobs follow the page layout.
EOF

cat > "$PKG/README.txt" <<'EOF'
QDelay 1.3.9 - dual dub delay for Akai MPC
==========================================

A stereo dual delay built for dub: independent tempo-synced taps, normal /
ping-pong / classic ping-pong routing, filtered and saturated feedback,
diffusion, modulation, tape wow and flutter, input ducking and a smooth
note-triggered Auto Clear.

Auto Clear (the AIR Delay Pro-style feature)
--------------------------------------------
Auto Clear is a continuous ducker, not a gate: the wet gain follows the note
detector smoothly, holds for Clear Hold, then releases over the Clear Fade
time. The gain scales the feedback as well as the output, so a duck really
removes energy from the loop instead of hiding it and letting the old tail
reappear when the gain opens again.

Clear Mode selects the behaviour: Transient ducks once per hit (fast/slow
detector, refractory) and is the default; Ducker follows the level smoothly
with a soft knee and cannot flutter. Clear in Feedback also scales the
feedback with the duck, so the tail really decays. At Clear Amt = 100% the
delay line is wiped once, when the wet path is effectively silent.

MPC effects receive no MIDI, so the MPC build always triggers from the audio
of the new note. Auto Clear is on by default; Dub Clean is the preset that
intentionally turns it off. Clear Thr is an absolute floor (-45 dB by default)
and the detector also reacts to a rise above the running level, so it works
over a pad or a busy mix, not only in silence.

Factory programs
----------------
Init, Dub Chamber, Tubby's Tape, Thunder Dub, Dub Siren, Melodica Echo,
Spring Dub, Dub Plate, Auto Clear Throw, Classic Pong, Dub Clean.

Controls are grouped across five pages: DELAY, TONE, SPACE, CLEAR and EQ.
The hardware Q-Links follow the page layout; use the page function keys to
switch pages. Feedback EQ is a low shelf at 120 Hz, a bell at 800 Hz and a
high shelf at 4.5 kHz.

OS 2x (SPACE page) runs the output limiter at twice the sample rate, so a hot
feedback sum cannot fold the clipper harmonics back into the audible band.
It adds 6 samples of latency, reported to the host. The knee sits at
-0.35 dBFS, so below it the limiter is bit-transparent.

Signal path:
  input -> diffusion (optional) -> dual delay -> feedback EQ / low+high cut
        -> saturation / tone -> back into the delay
  wet -> Auto Clear / duck -> stereo width -> mix -> output
      -> 2x oversampled limiter
EOF

echo "== 6/6  validate and zip"
python3 "$MPC/tools/validate.py" "$PKG" --params "$MPC/params.json"

python3 - "$PKG" <<'PY'
import os, shutil, sys
pkg = sys.argv[1]
root = os.path.dirname(os.path.dirname(pkg))
archive = os.path.join(root, "Tilr - VST - QDelay-1.3.9")
shutil.make_archive(archive, "zip", os.path.dirname(pkg), os.path.basename(pkg))
print("wrote " + archive + ".zip")
PY

echo
echo "MPC plugin folder: $PKG"
file "$PKG/VST/qdelay-arm64.so" "$PKG/VST/qdelay-arm32.so"

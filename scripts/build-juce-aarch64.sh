#!/usr/bin/env bash
# Build the JUCE VST2 GUI version for Linux AArch64 (desktop / ARM Linux).
# The MPC build is scripts/build-mpc.sh; MPC has no native plugin windows.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

IMAGE="qdelay-juce-aarch64"
BUILD="$ROOT/build-aarch64"
OUT="$ROOT/dist/juce-linux-aarch64"

command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }

echo "== 1/4  build the cross-compiler image"
docker build -q -t "$IMAGE" -f docker/Dockerfile.juce-aarch64 .

echo "== 2/4  configure (arm64 multiarch)"
if [ -d "$BUILD" ] && [ ! -w "$BUILD" ]; then
    docker run --rm -v "$ROOT":/work/src "$IMAGE" rm -rf /work/src/build-aarch64
fi
rm -rf "$BUILD"
mkdir -p "$BUILD"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$ROOT":/work/src -v "$BUILD":/work/build -w /work \
    "$IMAGE" bash -lc '
set -euo pipefail
cmake -S /work/src -B /work/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/work/src/cmake/toolchains/linux-aarch64.cmake \
  -DBUILD_STANDALONE=OFF -DBUILD_VST3=OFF -DBUILD_LV2=OFF -DBUILD_VST2=ON
'

echo "== 3/4  build"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$ROOT":/work/src -v "$BUILD":/work/build -w /work \
    "$IMAGE" bash -lc 'cmake --build /work/build -j"$(nproc)"'

echo "== 4/4  package"
mkdir -p "$OUT"
cp "$BUILD/QDelay_artefacts/Release/VST/libQDelay.so" "$OUT/QDelay-vst2-aarch64.so"
cp "$ROOT/mpc/assets/qdelay-bg.png" "$OUT/qdelay-bg.png" 2>/dev/null || true

cat > "$OUT/README.txt" <<'EOF'
QDelay 1.3.0 - JUCE VST2, Linux AArch64 (desktop GUI build)
==========================================================

QDelay-vst2-aarch64.so is the full JUCE plugin with the desktop editor shown
in sketch.png: dual delay, filter/EQ, diffusion, modulation, saturation, tape,
ducking, Auto Clear and the Dub preset bank.

Install on a 64-bit ARM Linux desktop / ARM DAW host:

    mkdir -p ~/.vst
    cp QDelay-vst2-aarch64.so ~/.vst/

This build is for ARM64 Linux desktops. It is NOT the Akai MPC build: MPC has
no plugin windows and no X11/font stack, so the MPC version is a separate,
headless VST2 with a touchscreen JSON skin. Use the folder

    dist/mpc/Tilr - VST - QDelay

on MPC hardware (MPC OS 3.10.1+, External Plugin Support enabled).

Dependencies: glibc 2.35 or older (the binary requires at most 2.35),
libstdc++ is linked statically; freetype/fontconfig are the normal desktop
libraries. X11/ALSA are loaded at runtime by JUCE.
EOF

echo
echo "JUCE ARM64 VST2: $OUT/QDelay-vst2-aarch64.so"
file "$OUT/QDelay-vst2-aarch64.so"
echo "NEEDED:"
readelf -d "$OUT/QDelay-vst2-aarch64.so" | grep NEEDED || true
echo "max glibc:"
readelf --version-info "$OUT/QDelay-vst2-aarch64.so" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1

# QDelay for Akai MPC standalone

This folder contains the MPC / VST2 build of QDelay. It is a separate,
headless build because MPC OS has no plugin windows: the on-screen UI is a JSON
skin (`Plugin Skins/TUI.json`) and the 16 hardware knobs are mapped with
`Q-Links.json`. Both are generated from `ui.json`.

| Path | Purpose |
|---|---|
| `src/QDelayMpc.cpp` | DSP and VST2 entry point (MPC Plugin SDK framework) |
| `framework/` | vendored MIT MPC Plugin SDK framework and DSP helpers |
| `tools/host-sim/` | behaviour-compatible MPC VST2 host used for testing |
| `tools/gen_ui.py` | generates the touch UI and Q-Link map from `ui.json` |
| `tools/validate.py` | checks the packaged folder, ELF, glibc and skins |
| `ui.json` | five pages: DELAY, TONE, SPACE, CLEAR, EQ |
| `plugin.json` | MPC manifest (name, vendor, uid, binaries) |
| `assets/` | page artwork used by the skin |

Build and package from the repository root:

```bash
./scripts/build-mpc.sh
```

The script builds a native copy for the x86_64 host simulator, runs the MPC
load/processing/state tests, generates the skin, cross-compiles
`qdelay-arm64.so` and `qdelay-arm32.so` in Docker, validates the package and
writes `dist/Tilr - VST - QDelay-1.3.1.zip`.

The MPC framework is MIT licensed; see `vendor/MPC-PLUGIN-SDK-LICENSE`.
The VST2 ABI in `framework/mpc_vst2_abi.h` is a clean-room description and does
not use Steinberg SDK headers.

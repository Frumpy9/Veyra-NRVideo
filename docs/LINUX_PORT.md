# Native Linux port — work in progress

Target: Linux Mint 22.x, RTX 3070, PS5 Remote Play with frame interpolation.
Fork baseline: upstream `fe8df5328a62849eb2d79053564b9574ab68aecd` (2.2.0).
Branch: `linux/remote-play`.

This is an implementation in progress, **not a working Linux Veyra player yet**.
The existing OBS filter remains enabled by default and unchanged. Windows
targets remain on their existing build path. No proprietary binaries, models,
credentials, or GPU driver modifications are included.

## Build the first milestone

On a development system with CMake 3.24+, a C++20 compiler, Vulkan development
headers/loader and `glslangValidator`:

```sh
cmake -S . -B out/linux-player -G Ninja \
  -DVEYRA_BUILD_OBS_FILTER=OFF -DVEYRA_BUILD_LINUX_PLAYER=ON \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build out/linux-player --parallel 2
ctest --test-dir out/linux-player --output-on-failure --timeout 120
```

Mint/Ubuntu development package names: `cmake ninja-build g++ libvulkan-dev
glslang-tools`. `mesa-vulkan-drivers` supplies software Vulkan for correctness
checks; it is not a substitute for NVIDIA hardware performance testing.

The executable `out/linux-player/linux/player/veyra_linux_interpolate` reads two
same-size binary RGB PPM images and writes an intermediate frame:

```sh
out/linux-player/linux/player/veyra_linux_interpolate A.ppm B.ppm middle.ppm 0.5
```

It refuses to overwrite an existing output. Optional final argument: shader
directory (otherwise the build's shader directory). Inputs are full-range SDR
RGB, dimensions 1..4096. HDR and YUV conversion are not implemented here.

## Implemented

- Separate, opt-in native Vulkan C++20 target; no D3D or Windows headers.
- Bidirectional 8x8 block motion search, limited to a 0..16 pixel radius.
- GPU intermediate-frame synthesis with inverse warping, bilinear sampling,
  forward/backward consistency and conservative endpoint fallback.
- Residual-based scene-cut gate; no generated dissolve across detected cuts.
- Bounded resources, explicit host/compute and compute/compute synchronization,
  input validation and RAII cleanup including partial initialization failures.
- GPU tests against known translated-image ground truth, static/endpoint
  preservation, scene cuts, invalid inputs and partial/tiny workgroups.
- Actual Veyra Remote Play core (queues, timestamps, input gating, decoder pump,
  recovery) compiled on Linux; its existing 79 tests pass without a console.
  Shared CMake declarations are extracted unchanged from the Windows target.

The search is a **reference implementation**, not production optical flow.
Large motion, ambiguous textures, occlusions, HUDs and cuts between similar
images need broader testing and better flow. Fallback can repeat an endpoint;
the program does not claim every output is a unique valid generated frame.

The PPM tool deliberately uploads and reads back pixels and waits for a fence.
It is an offline diagnostic boundary, not the live playback design. It must not
be inserted as a CPU round trip into the normal rendering path. Its elapsed
time includes submission/wait/readback and may include shader warmup; it is
neither GPU-only time nor display FPS nor end-to-end latency.

## Remaining work and acceptance gates

1. **GPU presentation and file playback:** Vulkan swapchain, GPU-resident output,
   bounded in-flight work, FFmpeg decode, explicit color conversion, source PTS
   scheduling, reset on source/size/PTS discontinuities; no CPU pixel readback in
   live rendering. Test pause/resume, resize, stalls and EOF.
2. **Remote Play:** reuse Chiaki transport/controller/audio components, preserve
   its licensing and pinned-source patches, adapt decoded-frame ownership and
   color metadata to Linux. Avoid desktop recapture. Registration/credentials
   remain local; do not initiate a real console session during automated tests.
3. **Frame pacing and motion quality:** improve flow with a pyramid/refinement
   path, scene changes, occlusion handling and HUD protection. Correct A/B
   presentation timeline and audio synchronization take precedence over a high
   submitted-frame counter. Interpolation needs B to arrive and adds lookahead.
4. **RTX 3070/Mint validation:** actual GPU throughput, memory use, sustained
   1080p, controller/audio operation, physical presentation cadence and latency.
   A software Vulkan pass does not establish these properties.
5. **Interface and packaging:** reuse portable Veyra interface/settings/effects
   where feasible, provide a runnable Linux distribution and repeatable builds.
6. **Neural enhancement:** independently verify available model/runtime paths
   on Ampere/Linux. Native support versus a Wine helper remains an open decision.
   No promise of DLSS/NR/Windows feature parity or proprietary redistribution.

The requested outcome remains a usable Linux Remote Play application with
switchable frame interpolation, then validated enhancement features. Passing
the current compute tests does not complete that outcome.

## Evidence, 2026-11-21

Local build on Mint 22.3, GCC 13.3, Vulkan headers 1.3.275, glslang 14.0.
Actual execution: Mesa llvmpipe (LLVM 20.1.2), including a run with Khronos
validation enabled. All 12 translated-image/phase cases had zero mean absolute
RGB error in the tested interior; static/endpoints/cuts/input bounds/tiny
extents passed. All 79 existing Remote Play core tests passed on Linux. No
validation errors were emitted. NVIDIA device access is
unavailable in the execution environment; no RTX 3070 measurement or PS5 session
has been performed. Raw logs are outside Git under the workspace's
`logs/linux-port/`. CI verification is separate and must be checked after push.

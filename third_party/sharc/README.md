# SHaRC (NVIDIA RTXGI): source of the port

`src/sharc.wgsl` is a function-by-function WGSL port of `include/HashGridCommon.h` and `include/SharcCommon.h` from
NVIDIA's SHaRC SDK, version 1.8.3 (https://github.com/NVIDIA-RTX/SHARC, commit 4e21b58).

This software contains source code provided by NVIDIA Corporation.

License: NVIDIA RTX SDKs License, copied in `License.md`.

Default parameters taken from the Pathtracer sample of the RTXGI repository (https://github.com/NVIDIA-RTX/RTXGI,
`Samples/Pathtracer/UI.h` and `Pathtracer.cpp`): sceneScale 50, 20-frame accumulation, eviction after 60 frames
without samples, update of one pixel per 5x5 block, 2^22 entries.

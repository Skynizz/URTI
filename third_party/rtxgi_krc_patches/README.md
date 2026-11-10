# `krc` branch of NVIDIA's RTXGI SDK

Patches to apply on the RTXGI SDK (NVIDIA-RTX/RTXGI repository, `main`): `git am *.patch` at the SDK root, then
`git am donut/*.patch` in `External/Donut`. They add the measurement bench (`KrcBench.cpp`), KRC-A, guiding (negative
result), exact relighting and the `Libraries/Krc` library. Measurement tools: `tools/rtxgi/`.

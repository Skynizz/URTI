# SHaRC (NVIDIA RTXGI) — source du portage

`src/sharc.wgsl` est un portage WGSL, fonction par fonction, de
`include/HashGridCommon.h` et `include/SharcCommon.h` du SDK SHaRC de NVIDIA,
version 1.8.3 (https://github.com/NVIDIA-RTX/SHARC, commit 4e21b58).

This software contains source code provided by NVIDIA Corporation.

Licence : NVIDIA RTX SDKs License, copiée dans `License.md`.

Paramètres par défaut repris de l'exemple Pathtracer du dépôt RTXGI
(https://github.com/NVIDIA-RTX/RTXGI, `Samples/Pathtracer/UI.h` et `Pathtracer.cpp`) :
sceneScale 50, accumulation 20 frames, éviction après 60 frames sans échantillon,
mise à jour d'un pixel par bloc 5×5, 2^22 entrées.

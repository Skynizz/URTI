# Branche `krc` du SDK RTXGI de NVIDIA

Patches à appliquer sur le SDK RTXGI (dépôt NVIDIA-RTX/RTXGI, `main`) : `git am *.patch` à la
racine du SDK, puis `git am donut/*.patch` dans `External/Donut`. Ils ajoutent le banc de mesure
(`KrcBench.cpp`), KRC-A, le guidage (résultat négatif), le rééclairage exact et la bibliothèque
`Libraries/Krc`. Outils de mesure : `tools/rtxgi/`.

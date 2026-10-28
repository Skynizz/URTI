# État de l'art utile à KRC (octobre 2026)

Ce que font les meilleurs travaux récents, ce qui en est repris dans KRC, et ce qui reste propre à
KRC. À compléter avant toute revendication de nouveauté.

## Caches de radiance en espace monde

- **SHaRC** (NVIDIA, SDK RTXGI v2, v1.8.3) : table de hachage en espace monde, taille des cellules
  selon la distance à la caméra, mise à jour par 1 pixel sur 25, lecture au point touché ; option
  harmoniques sphériques (direction de la lumière), mélange des niveaux voisins quand la caméra
  bouge. C'est la base de comparaison de nos bancs.
  <https://github.com/NVIDIA-RTX/RTXGI>
- **NRC** (Müller et al., SIGGRAPH 2021) : cache neuronal appris en continu ; peu utile pour les
  matériaux diélectriques (verre). <https://arxiv.org/abs/2106.12372>
- **Neural Two-Level Monte Carlo / NIRC** (CGF 2025) : cache de la radiance *incidente*, et
  estimateur à deux niveaux : le cache donne l'essentiel, une correction rare et sans biais retire
  son erreur. Cousin direct de notre variable de contrôle (v2) : une piste pour corriger le biais du
  cache (fuites, écrêtage) au lieu de le rustiner. <https://arxiv.org/abs/2412.04634>
- **World-space ReSTIR** (GPUOpen, SIGGRAPH Asia 2021) : réservoirs rangés dans une table de hachage
  en espace monde. <https://gpuopen.com/download/SA2021_WorldSpace_ReSTIR.pdf>
- **Bevy Solari** (2025) : ReSTIR DI + ReSTIR GI + cache d'irradiance en espace monde ; le cache garde
  l'ancienne lumière quand les lampes s'éteignent (retard), problème que KRC traite.
  <https://jms55.github.io/posts/2025-12-27-solari-bevy-0-18/>

## Guidage des chemins

- **Real-Time Markov Chain Path Guiding** (Alber, Hanika, Dachsbacher, I3D 2025) : guidage temps
  réel sans biais, mélanges de von Mises-Fisher rangés dans des tables de hachage en espace monde,
  démo Quake en path tracing. Le guidage par cellule n'est donc pas une nouveauté de KRC.
  <https://cg.ivd.kit.edu/english/markov-chain-path-guiding.php>
- **Markov Chain Mixture Models for Real-Time Direct Illumination** (Dittebrandt et al., EGSR 2023) :
  la version en espace écran. <https://cg.ivd.kit.edu/english/mcmm.php>
- **RCPG** (guidage produit par cascades de radiance) : <https://dl.acm.org/doi/10.1145/3840291>

## Mesure

- **ꟻLIP** (NVIDIA Research, HPG 2020) : erreur perçue en alternant deux images, LDR et HDR.
  <https://github.com/NVlabs/flip>

## Ce qui reste propre à KRC (à vérifier dans la littérature avant de le revendiquer)

- Chemins corrélés avant / après un changement de lampes, avec Δ = 0 exact là où rien ne change.
- Rééclairage exact par groupes de lampes, appliqué aussi à l'historique du débruiteur ; pour un
  débruiteur fermé (NRD), par normalisation de son entrée par le produit des facteurs de la cellule.
- Guide figé pendant un événement (même loi de tirage pour l'ancienne et la nouvelle scène), pour
  que les chemins corrélés restent exacts tout en étant guidés.
- Taille des cellules décidée par un test statistique (cellule fine / mère, rétrécissement).

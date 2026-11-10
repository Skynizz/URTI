# URTI — Universal Ray Tracing Injector

URTI ajoute du ray tracing en temps réel à des jeux DirectX 12 qui n'en ont pas, sans modifier leurs
fichiers ni leurs shaders. C'est un add-on [ReShade](https://reshade.me) : il observe le rendu du jeu,
reconstruit la scène de son côté et y lance de vrais rayons (DXR 1.1).

## Ce que ça ajoute

- **Occultation ambiante** en ray tracing, dans la vraie géométrie de la scène, hors écran compris.
- **Rebond de lumière** diffus, à partir des couleurs déjà vues à l'écran.
- **Reflets** sur l'eau et les surfaces mouillées, stabilisés dans le temps.

## Comment ça marche

1. **Capture de la géométrie.** Chaque dessin du jeu est rejoué par sortie de flux : URTI récupère les
   triangles tels que les vertex shaders les ont transformés (animation et instanciation comprises),
   sans rien décoder.
2. **Scène RT.** Une structure d'accélération par image pour le proche et le mobile, plus un cache
   d'objets pour tout le reste, repositionné grâce à la caméra du jeu.
3. **Rayons.** Passes de calcul en ray queries : occultation, rebond, reflets. Chaque passe a un budget
   de temps tenu à chaque image.
4. **Composition.** Le résultat, débruité et stabilisé, est posé dans l'image du jeu. L'interface reste
   intacte quand le jeu fournit une image sans interface.

L'historique détaillé des essais et des mesures est dans [docs/JOURNAL.md](docs/JOURNAL.md).

## Jeux testés

| Jeu | Effets | Réglages (`urti.ini`) |
|---|---|---|
| Red Dead Redemption 2 | occultation, rebond, reflets sur surfaces mouillées | `ui_photo=0`, `ui_zone=0.012,0.69,0.16,0.97`, `jitter=0`, `shadow_rays=2` |
| Horizon Forbidden West (Complete Edition) | occultation, rebond, reflets sur l'eau | `camera_cb=512`, `material_slot=4`, `reflect_channel=0`, `reflect_invert=1`, `smooth_min=0.9`, `ao_rough=1`, `person_mask=0` |

Dans Horizon Forbidden West, en 2560×1440 avec DLSS et génération d'images sur RTX 4070 : 134 images par
seconde sans URTI, 74 à 94 avec, selon la scène.

**Mode histoire uniquement.** URTI s'injecte dans le rendu du jeu : ne l'utilisez jamais en ligne.

## Installation

Il faut une carte graphique compatible DXR 1.1 et [ReShade](https://reshade.me) 6.8 **avec prise en charge
des add-ons**.

1. Installez ReShade dans le dossier du jeu (ou avec `tools/reshade/install.py`, réversible avec `--remove`).
2. Copiez `urti.addon64` à côté de l'exécutable du jeu.
3. Créez `urti.ini` au même endroit, avec `live=1`, `ao=1`, `reflect=1` et les réglages du jeu (tableau ci-dessus).

| Touche | Effet |
|---|---|
| F7 | active ou coupe URTI |
| F5 | écran partagé, sans et avec URTI |

## Compilation

Prérequis : Windows 10 ou 11, Visual Studio 2022 (C++), CMake 3.20 ou plus récent, et
[dxc](https://github.com/microsoft/DirectXShaderCompiler).

```sh
cmake -S urti -B urti/build -DURTI_DXC="C:/chemin/vers/dxc.exe"
cmake --build urti/build --config Release
```

L'add-on est produit dans `urti/build/Release/urti.addon64`.

## Réglages utiles

| Clé | Rôle | Défaut |
|---|---|---|
| `ao_ms` | temps visé pour l'occultation et son débruitage (ms, 0 : pas de budget) | 4 |
| `refl_ms` | temps visé pour les reflets (ms, 0 : pas de budget) | 2.5 |
| `ao_radius` | portée de l'occultation (m) | selon le jeu |
| `fg_shift` | déplacement des effets sur les images générées (0 : figés) | 0.5 |
| `hudless` | image sans interface fournie par le jeu (0 : ignorée) | 1 |
| `reflect_all` | reflets aussi hors de l'eau (si la caméra du jeu est connue) | 0 |

## Contenu du dépôt

| Dossier | Contenu |
|---|---|
| `urti/` | l'add-on (C++, HLSL) et son banc d'essai |
| `reshade/` | effets ReShade associés |
| `tools/reshade/` | installation, essais automatisés en jeu, analyse d'images ([ꟻLIP](https://github.com/NVlabs/flip)) |
| `docs/` | journal, feuille de route, résultats, état de l'art |
| `src/`, `krc_sdk/` | KRC, le cache de radiance étudié en amont |
| `third_party/` | SHaRC et correctifs pour Quake II RTX, RTGL1 et RTXGI |
| `results/` | mesures et captures d'écran |

## KRC — Knowledge Radiance Cache

Le dépôt contient aussi le prototype de recherche à l'origine du projet : un cache spatial de radiance
où chaque cellule connaît son incertitude, et où les rayons vont là où l'on doute. Protocole dans
[docs/HYPOTHESE.md](docs/HYPOTHESE.md), mesures dans [docs/RESULTATS.md](docs/RESULTATS.md).

```sh
cargo run --release -- all      # références et toutes les méthodes
cargo run --release -- sponza 480
```

## Limites

- Chaque jeu demande un petit profil de réglages : le masque de l'eau et la caméra se trouvent par relevé.
- Les dessins par mesh shaders et la tessellation ne sont pas captés.
- Sans image sans interface fournie par le jeu, seule une zone fixe de l'écran est protégée.

## Licence

Le code d'URTI et de KRC est sous licence [MIT](LICENSE). Le code tiers garde la licence de son projet
d'origine : `third_party/`, `urti/third_party/` (ReShade, Dear ImGui), `reshade/ReShade*.fxh`.

Ce projet n'est affilié ni à Rockstar Games, ni à Guerrilla Games, ni à Sony, ni à NVIDIA, ni à ReShade.

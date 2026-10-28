# URTI — Universal Ray Tracing Injector

Couche intermédiaire qui observe un jeu DirectX 12, fabrique une scène parallèle et y lance des
rayons, sans modifier ses shaders. Plan complet et avis de faisabilité : docs/URTI.md.

## État

| Étape | Contenu | État |
|---|---|---|
| 0 | Banc : application DX12 en rastérisation à géométrie connue (`testapp/`), fenêtre cachée | fait |
| 1 | Capture de la géométrie par sortie de flux, test de couverture | **99,80 %** sur le banc ; jeu réel à faire |
| 2 | Caméra estimée, géométrie persistante en espace monde, BVH, ombres RT du soleil | à faire |
| 3 | Cache de lumière en espace monde (KRC), reflets et éclairage indirect | à faire |

## Principe de la capture (add-on ReShade, `addon/urti_addon.cpp`)

- Chaque pipeline graphique du jeu reçoit un pipeline « ombre » : même vertex shader et même
  signature racine, sortie de flux de `SV_Position`, sans rastérisation ni pixel shader.
- Pendant la frame capturée, chaque dessin est rejoué une fois avec ce pipeline, puis l'état du jeu
  est remis : rien ne change à l'écran. On obtient les triangles tels que le jeu les a transformés
  (animation sur GPU, instanciation, sommets compressés compris), sans rien décoder.
- D3D12 exige `ALLOW_STREAM_OUTPUT` dans la signature racine : l'add-on l'ajoute à celles du jeu à
  leur création (`ID3D12Device::CreateRootSignature`).
- Test de couverture : les triangles captés, rastérisés à nouveau, doivent reproduire la profondeur
  du jeu. Aucune caméra n'est nécessaire pour ce test.

Limites de la version 0 : dessins indirects (`ExecuteIndirect`) comptés mais pas capturés ;
pipelines avec geometry / tessellation / mesh shaders ignorés ; triangles qui traversent le plan
proche ignorés dans le test.

## Construire et tester

    cmake -S urti -B urti/build -A x64 && cmake --build urti/build --config Release
    # dans urti/build/Release : ReShade 6.8.0 « Addon » renommé dxgi.dll, ReShade.ini, urti.addon64
    URTI_FRAMES=90 URTI_CAPTURE_FRAME=60 ./urti_testapp.exe   # journal : urti_log.txt

En-têtes tiers : `third_party/reshade` (API d'add-ons de ReShade 6.8.0, BSD-3), `third_party/imgui`.
Jeux : mode solo uniquement (la version « Addon » de ReShade est refusée en ligne).

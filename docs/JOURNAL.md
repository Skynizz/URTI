# Development journal

Steps v1 to v24 (KRC, the radiance cache studied first) are in the [KRC](https://github.com/Skynizz/KRC) repository.

## v25 — URTI, step 1 validated in Red Dead Redemption 2

- URTI (Skynizz's idea: a universal ray-tracing injector) is a ReShade add-on that captures the geometry of a DX12 game
  without touching its rendering. There is one "shadow" pipeline per game pipeline: same vertex shader, stream output
  of SV_Position, no rasterisation. Draws are replayed during the capture, including `ExecuteIndirect` (intercepted
  directly). `ALLOW_STREAM_OUTPUT` is added to the game's root signatures. DX12 bench with known geometry: 99.80 %
  coverage.
- RDR2 (DX12, OptiScaler, DLSS and DLSS-G frame generation, 2560×1440, rendered at 1706×960): **97.5 to 97.6 % of the
  game's depth is reproduced** by the captured geometry (3 captures, mean deviation 1e-4), animated characters
  included. The step's criterion (> 95 %) is met. Scene: the story mode loading scene, rendered by the engine (~3,300
  draws, 520,000 to 650,000 triangles).
- Pitfalls of the real game, one per attempt: several D3D12 devices (crash on F9); one present out of two without any
  draw (frame generation); vertex budget too large; successive draws overwriting each other in a shared stream
  (barrier); depth never visibly cleared (ReShade's copy); full-screen lighting passes taken for geometry (pipelines
  without depth writes excluded).
- Tests run without Skynizz: launch, menu (click on "Story"; injected Enter is ignored), captures through a trigger
  file (`tools/reshade/urti_rdr2.ps1`). Story mode only.
- Left for step 2: camera (matrices) to get to world space, persistent geometry, BVH, RT sun shadows; a test in the
  open world (not only the loading scene).

## v26 — URTI, step 2a: the camera found without knowing anything about the game

- Richer capture (`.urti` file): signature and bounds of each draw in the stream (start and end read from the GPU
  counter), depth. Offline analysis: `urti/tools/analyse.py`.
- **Self-calibration**: clip-space w gives the depth along the view axis; with the field of view, each vertex becomes a
  camera-space point. A still object seen in two frames must move by a single rigid motion: we look for the most rigid
  field of view (robust fit; objects that move on their own are discarded).
- Bench (orbiting camera, ground truth): 60.15° for 60.16°, displacement 0.390 for 0.390.
- RDR2 in game (injected mouse to turn the camera): 36.95° and 36.50° on the two captures where the camera turns by
  more than 1°; 38.8 to 39.1° when it barely turns (poorly constrained). No ground truth in the game; the consistency
  of the estimates is the only check.
- In-game coverage, moving camera, over 8 presents: 70 to 94 % (geometry from several frames is compared with the depth
  of the last one); to be redone frame by frame.

## v27 — URTI, step 2b: sun found, first ray-traced shadows in RDR2

- The bench now has a sun and a shadow-map pass, like a game. The add-on captures every depth stream (screen and shadow
  maps); the `.urti` file moves to version 2.
- **Sun direction**: the same mesh seen by the camera and in the shadow map (parallel projection) is linked by an
  affine map; the direction it sends onto the map's depth axis is the sun's. Fit per pair of draws, then a vote (a mesh
  appears in several cascades of an atlas). Bench: 0.01° off. RDR2: the 2048×8192 cascade atlas gives a clear direction
  (82 to 84 % of pairs agree); the small maps (1024², 512², 64²) are vertical views (rain, occlusion), not the sun.
  Sign: the one that leaves the scene lit (the other puts everything in shadow, sun below the horizon).
- **Ray-traced shadows** (`urti/tools/urti_rtshadow`, DXR 1.1, inline queries): geometry of the last frame in camera
  space (linear in clip space), acceleration structure, pixel rebuilt from the game's depth (depth = A + B / w fitted on
  the vertices), ray toward the sun. The result does not depend on a field-of-view error (affine map: intersections are
  preserved). Bench: correct cast shadows. RDR2 (1.7 M triangles, 38 MB BLAS): consistent shadows (lit shoulders, the
  character's cast shadow, the pier's structure); hazy scene, few sharp game shadows to compare with.
- Field of view by self-calibration in RDR2: unstable (51.5° here, 37° before) when the camera barely turns; no effect
  on shadows, but it must be made reliable for lighting.

## v28 — URTI, live ray-traced sun shadows in RDR2

- Live mode (`live=1` in `urti.ini`, F7: on / off, F6: recalibrate): every frame, the screen geometry captured by stream
  output becomes an acceleration structure (BLAS rebuilt, 4.6 to 8.4 M vertices in game), then one ray per pixel of the
  game's depth goes toward the sun (DXR 1.1, inline queries). The `urti.fx` effect darkens shadowed pixels (strength
  0.55, split screen available). Sun and depth law recalibrated every 120 frames.
- Screenshots: the game displays in HDR10; they are read from the display buffer after the effects, PQ decoded,
  BT.2020 → BT.709, white at 203 nits. Also fixed: F7 left the old shadow image on screen.
- RDR2 (Saint Denis station, `results/urti/rdr2_direct/`): the RT shadows fall in the same place as the game's (the
  station canopy on the ground), which validates the chain (geometry, sun, depth). But the game already has its
  shadows: the darkening adds on top and makes already-shadowed areas too black. Faces turned away from the sun
  (backlit wagons) are also blackened while the game already lights them less.
- In the rain, overcast sky: the game draws no sun shadow map, so there is no sun to find and no RT shadows (intended).
- Cost: about 124 → 81 frames per second (DLSS frame generation on), i.e. −35 %.
- Next: do not darken what is already in shadow in the game's image (compare luminance with lit neighbours, or act
  only on pixels where the game's shadow is missing: distance, small objects), ignore faces turned away from the sun
  (normals from depth), update the BLAS instead of rebuilding it to reduce cost.

## v29 — URTI: stable RT shadows in RDR2, frame generation, hot reload

- **Sun measured every frame on the GPU**: pairs of draws screen / sun shadow map with the same signature, affine fit
  per pair, vote; 220 to 250 pairs per frame in RDR2, measured on 90 to 100 % of frames. No more heavy periodic
  calibration (only at start and on F6). The sun's orientation comes from the map's depth convention (set once), no
  longer from the previous frame: F7 off/on no longer sends shadows in every direction.
- **Frame generation (OptiScaler, DLSS-G)**: generated frames are built from the game's image before ReShade, hence the
  "native / RT" alternation. Shadows are now laid by the add-on on every presented frame (300 / 300, ~140 of them
  generated), in linear light (HDR10 PQ decoded).
- **Game shadow recomputed** from its shadow map (cascades identified by their viewport, camera → map mapping measured
  every frame). Mode "only the shadows the game lacks" and mode "full RT shadows" (more contrast), chosen in the ReShade
  menu (UrtiMode). Finding: RDR2's shadows are already precise; RT mostly brings fine detail (wheels, the horse's legs)
  and contrast in full mode.
- Faces turned away from the sun (normal from depth): no RT shadow added in combined mode (spots on backlit facades
  removed). Four rays per pixel within the sun disc (0.5°) and a 3×3 filter: soft shadow edges.
- RDR2 has no separate shadow mask: pass survey (F8) with snapshots taken at the end of the writing of each target;
  G-buffer found (normals, materials) for reflections.
- **Fast tests**: settings (`urti.ini`) and shaders (`urti_shaders/*.cso`, `tools/reshade/urti_shaders.sh`) hot-reloaded
  in game in ~2 s; launch driven by the log (`urti_rdr2_rapide.ps1`).
- Fixed along the way: `&` on a `ComPtr` (releases it), effect parameters removed by ReShade's compiler when no longer
  read, barriers without a resource, ReShade → D3D12 states.

## v30 — URTI: live-mode fixes, first RT reflections

- Shadows frozen on screen when the camera moves: the mask was applied even when it could not be recomputed
  (calibration, missing depth, game frame taken for a generated one). The mask is now applied only if it dates from
  this frame or the previous one; generated frame = present without any game draw.
- ReShade menu: the URTI effect unchecked, or effects turned off, now remove RT shadows (the add-on applied them itself
  without checking); mode, strength, split screen read and logged.
- Without a sun (night, indoors): calibrations spaced out (1×, 2×, 4×… the interval), depth law kept alone:
  reflections work at night too.
- **RT reflections**: one reflected ray per smooth pixel (normal from depth), hit point reprojected and checked
  visible, colour read from the image, Fresnel. Validated on the bench (floor reflecting the cubes,
  `results/urti/banc_direct/reflets_rt.png`). In RDR2, the G-buffer is located and its material image copied at the end
  of its writing every frame; still to find which channel holds smoothness (channel sweep through hot reload, to redo
  in front of water or a wet street).
- GPU lost in RDR2: the composition read the buffer of disabled reflections beyond the buffer bound in its place; an
  out-of-bounds read of a root buffer is not bounded. Fixed.

## v31 — URTI: capture without waiting, temporal stability (steps 1 to 4)

- Capture: one region of the stream and one counter per draw (no more barrier per draw): from ~20 to ~50 fps in RDR2.
  Regions rounded to three vertices (otherwise triangles formed across two objects: large ground triangles that
  appeared and disappeared).
- Instant start: stored calibration (`urti_calib.ini`: shadow-map convention and description, motion-vector
  convention), depth law measured every frame on the GPU, slot reserved for the sun's shadow map.
- Sun: vote refined to 0.75°, small jumps rejected, large jumps accepted only once confirmed.
- The game's motion vectors copied from its G-buffer, convention found on its own (16 candidates, using the previous
  frame's depth): RDR2 = pixels, forward direction.
- Shadows: separate denoising pass (3×3 blur guided by depth, reprojected history, clamp to mean ± 1.5 σ, adaptive
  blend 0.1 to 0.5 depending on motion), ray pattern rotating from frame to frame, shadow weighted by N·L (removes only
  direct light), faded out in the distance (haze).
- Reflections: stabilised (history, clamp to ± σ, adaptive blend, capped colour) and reprojected onto the reflected
  point: camera rotation measured from frame to frame through the cascades (linear part of L_prev⁻¹ L_cur; the
  cascades' translation does not affect it), translation derived from the surface's motion vector and the virtual
  depth (distance + hit distance).
- Nine diagnostic views (ReShade menu and `debug_view`), generated frames (DLSS-G) optional.

## v32 — URTI: "AAA game" sandbox (urti_sandbox)

A small D3D12 game that reproduces RDR2's conditions, to test URTI in a few seconds instead of launching the real game
(`urti/sandbox/main.cpp`, `tools/reshade/urti_sandbox.sh`).

- Rendered like a recent game: deferred G-buffer at 2/3 resolution with sub-pixel jitter (like DLSS), same formats as
  RDR2 (RGBA8 material with smoothness in channel 1 of target 2, R16G16 motion vectors in pixels), reversed infinite
  depth D32S8, half-resolution depth, atlas of 4 cascades 2048×8192 D16 following the camera, full-screen lighting,
  upscaling, HDR10 output (PQ). Root signature without stream output, ExecuteIndirect, optional frame generation.
- Hard scene: a street, buildings with smooth windows, puddles, thin poles and cables hanging and swinging in the wind,
  trees (branches and leaves in the wind), carts and walking passers-by, fences (one post = one draw), barrels. 226
  objects, ~1,200 draws per frame. Scripted camera (walking, looking around, fast U-turns), identical on every launch;
  frame rate locked at 90 fps.
- URTI recognises it as a game: exact depth law (0.15 / w = near plane), sun on 98 % of frames, cascades, camera
  rotation, motion vectors in the same convention as RDR2 (2).
- Screenshots at precise presents (`photo_frames=800-830` in urti.ini); `--photos` runs two identical launches (RT then
  native) and `urti_sandbox_compare.py` measures the RT effect, the added flicker and the map of jumping points.
  Reference: effect 2.1 %, added flicker +0.16 %, 0.9 % of pixels jumping, concentrated on distant reflections in the
  puddles — the defect seen in RDR2 (`results/urti/bac_a_sable/`).
- Full test (two launches and measurements): ~30 s.

## v33 — URTI: suite of 7 tests, ground truth, geometric anchoring tried

- Sandbox closer to a game: TAA (reprojected, clamped history, upscaling) — the native image is stable from frame to
  frame as in RDR2; adjustable wet street (`URTI_SANDBOX_WET`), moving objects frozen on demand, a wall for occlusion,
  sub-pixel jitter can be turned off (`URTI_SANDBOX_JITTER=0`).
- **Ground truth** (`URTI_SANDBOX_TRUTH=1`, `--verite`): the floor reflects the exact scene (rendered mirrored), blended
  with the same rule as URTI (Fresnel f0 0.04, smoothness > 0.35, cap 4). We measure how far RT reflections are from
  the ideal image; a reflection that moves normally no longer counts as a jump.
- New measurement: **blinking pixels** (the deviation from the reference changes sign from one frame to the next),
  distinct from an error that moves with the scene.
- `tools/reshade/urti_sandbox_suite.sh`: still building, still tree, tree in the wind, character, fixed camera, moving
  camera, occlusion (~3 min 30). Reference (`results/urti/bac_a_sable/suite/`):

  | scene | deviation | added | jumping | blinking |
  |---|---|---|---|---|
  | building | 3.31 % | +0.015 % | 0.012 % | 0.101 % |
  | still tree | 2.97 % | +0.036 % | 0.084 % | 0.163 % |
  | tree in the wind | 2.94 % | +0.044 % | 0.086 % | 0.164 % |
  | character | 2.75 % | +0.033 % | 0.191 % | 0.093 % |
  | fixed camera | 1.19 % | +0.049 % | 0.144 % | 0.118 % |
  | moving camera | 1.15 % | +0.044 % | 1.257 % | 0.871 % |
  | occlusion | 1.99 % | +0.024 % | 0.181 % | 0.093 % |

- Diagnosis of the walk (the only bad case): half of the jumps come from RT shadows (0.59 % without them), the rest
  from reflections around the horizon. Ruled out by measurement: sub-pixel jitter (same result without), clamping,
  depth tolerance and history share (0.59 % ± 0.03 whatever the setting), synchronisation of the two launches
  (checked), composition (empty test: 0.000 %). The ground truth has its own aliasing (0.49 % blinking against native):
  that is the measurement's floor.
- **Geometric anchoring** of reflections (`anchor_alpha=`): stable identity per draw (signature + rank), previous
  frame's vertices kept, mirror and reflected point found in the captured geometry (same triangle, same barycentrics,
  deformations included), previous frame's virtual point → exact place in the history, kept only if the same object
  was reflected in the same mirror. It works (two thirds of reflecting pixels, `ancrage_couverture.png`) but **does
  not reduce blinking**: 0.909 % against 0.871 % while walking, slightly worse with a fixed camera (switching from one
  filter to the other depending on the pixel adds some). Kept, off by default, free when off.
- Test settings in urti.ini: `refl_alpha_min`, `refl_alpha_max`, `refl_gamma`, `refl_depth_tol`, `anchor_alpha`,
  `anchor_gamma`.

## v34 — URTI: shadows and reflections no longer apply on top of each other

- Question: do RT shadows shake *through* reflections? The composition blended the reflection, then darkened
  everything with the shadow mask: a puddle in shadow had its reflection darkened (wrong: the shadow only removes the
  surface's direct light), and the shadow's shaking added to the reflection's.
- Measurement per event (pixel × frame, `tools/reshade/urti_sandbox_overlap.sh`: five launches, shadows alone against
  native, reflections alone and both against ground truth; events overlap, percentages are not subtracted) on the
  walk: jumps with both 1.257 %, explained by shadows 0.682 %, by reflections 0.513 %, **due to the overlap alone
  0.062 % (5 %)**. Shadows alone jump on 0.738 %: the biggest part.
- Fix: the shadow is applied before the reflection (which partly replaces it). Jumps 1.257 → 1.168 %, overlap 0.062 →
  0.037 %; across the 7 scenes, deviation from truth and jumps go down everywhere
  (`results/urti/bac_a_sable/suite/tableau_v34.txt`, `results/urti/bac_a_sable/superposition/`).

## v35 — URTI: alpha-tested foliage (holes recognised by the camera and by the sun)

- Why RDR2's foliage "jumps" and the sandbox's does not: games draw foliage (grass, fringes, fences) as cards whose
  texture cuts out the leaves (alpha test). URTI captures the geometry, not the texture: to it, each card is a full
  rectangle; a ray that hits a hole is rejected (the reflection disappears) or casts a rectangular shadow, and the wind
  makes it alternate.
- Sandbox: foliage as crossed cut-out cards (screen, shadow map, ground-truth mirror), `URTI_SANDBOX_LEAVES=blocs` for
  the old one; the tree test framed with its reflection in the puddle; realistic shadow map (depth over 400 m, bias of
  40 D16 steps instead of 600: ~25 cm instead of several metres). The defect is reproduced: the tree's reflection full
  of holes and made of rectangles (`results/urti/bac_a_sable/feuillage/`).
- Fix (`alpha_screen=1`, default): every triangle a ray meets is examined.
  - On screen: if the camera sees further than this point (or the sky), it is a hole: the ray goes on.
  - Off screen (shadows of the canopy above the camera): the game's shadow map is the sun's view, alpha test included;
    the point is a hole if the first occluder it recorded is at least halfway between it and the receiver (the game
    records its occluders a bit further than they are: a fixed threshold made every RT shadow disappear, seen in the
    mask and fixed). The game's map is therefore also copied outside combined mode (+0.17 ms).
- Measurements (with the cards): walk, jumps 1.294 → 1.177 %, blinking 0.952 → 0.853 %, deviation from truth 1.14 →
  1.01 %; tree, deviation 2.67 → 2.46 %. The 7 scenes: `suite/tableau_v35.txt` (the sandbox changed: foliage and shadow
  map, so earlier tables do not compare line by line).

## v36 — URTI: reflection "popping" fixed, acceleration structure half as expensive

- In-game feedback: reflections no longer go in every direction but appear and disappear in a loop, very fast. New
  diagnostic view 9 (share of the RT reflection actually applied) and a measurement of toggles
  (`tools/reshade/urti_sandbox_popping.sh`: the share changes by more than 0.3 from one frame to the next; round trip:
  one way, then straight back). Walk: 0.404 % round trips of reflecting pixels per frame, concentrated in foliage,
  cables and windows.
- Cause 1: the "wet floor" mode (on by default in RDR2) made smooth any surface whose depth normal points up; in foliage
  that normal is noise, and the pixel became reflective every other frame. Wet mode now applies only to a flat surface
  (points 4 pixels away on each side, on each axis, aligned with it): 0.404 → 0.276 %.
- Cause 2: a smooth surface whose reflected point is hidden or off screen (a window reflecting the other side of the
  street at a grazing angle, leaves) dropped to zero, then came back as soon as it was visible again. It is now missing
  data (negative weight): stabilisation keeps the history, which fades (× 0.9 per frame), and the clamp counts only
  known neighbours: 0.276 → 0.140 % (−65 % in total). Against ground truth, no trail: walk, jumps 1.177 → 1.133 %,
  blinking 0.853 → 0.802 %, deviation 1.01 → 0.98 %; other scenes equal or better (`suite/tableau_v36.txt`).
- Speed: the acceleration structure was built over the whole reserved capacity (8.4 million vertices in RDR2), empty
  part included; it now covers only the frame's vertices. And as long as the list of draws is the same as at the last
  build, it is updated (refit) instead of rebuilt, with a rebuild at least every 8 frames (`blas_refit=`, 0: always
  rebuild). Sandbox: 0.65 → 0.35 ms, identical image; the log now gives vertices per frame and the number of rebuilds
  and updates.

## v37 — URTI: geometry budget, learned sizes, the game's UI spared

- RDR2's log (v36) showed it: **8,388,605 vertices out of 8,388,608** — the capture slot is full every frame. Later
  draws are refused, but small ones still get in: the objects in the RT scene change from frame to frame (jumping
  shadows and reflections), the structure covers the whole capacity (no speed gain) and the list always changes
  (0 updates). The sandbox (122,000 vertices) could not show it.
- **ExecuteIndirect**: each draw reserved 65,536 vertices (count unknown before the draw). Size now learned per
  identity (measured, + 25 % and 64 of margin). Sandbox: demand goes from 122,000 to 60,000 vertices — half of the slot
  was empty.
- **Geometry budget** (`vertex_budget=`, 4 million by default; 0: none): each captured object is measured on the GPU
  (`live_regionstats.hlsl`: minimum distance and size in camera space); only the most important (apparent size) enter
  the RT scene, with a 30 % bonus for those already in (no back and forth); an object too big for the remaining room is
  skipped. Objects outside the budget are not captured at all (fewer replayed draws) and are re-measured in rotation in
  a measurement area at the end of the slot, outside the structure (`probe_budget=`, 1 million vertices per frame): an
  object you walk toward gets in. The budget applies only to the main screen (a second depth pass, like the sandbox's
  half-resolution one, changed the main screen); identities are computed before any refusal (they slid when the slot
  was full).
- Saturation reproduced (`slot_limit=`: smaller slot): walk, jumps 1.133 → 1.402 % without budget; with the budget, the
  image is closer to the ideal (deviation 1.16 → 1.08 %, added flicker +0.067 → +0.021 %). In the sandbox the refused
  draws are always the same; in RDR2 they change, and that instability is what the budget removes: to be confirmed in
  game (the log now gives reserved vertices, objects in the RT scene, out of budget, re-measured, refused).
- **The game's UI** (menus reflected on the ground): reflections read their colours from the final image, UI included,
  and the composition drew shadows and reflections over the menus. URTI now takes a snapshot of the screen at the first
  draw of each binding of the screen image (up to four per frame); `live_uicompare.hlsl` counts the pixels of each
  snapshot identical to the final image; the first snapshot more than 60 % identical is the image before the UI.
  Reflections read their colours from it, and the pixels that differ (the UI) are left untouched. Sandbox with a UI
  (minimap, bars, a menu open 2 s out of 4): snapshot 0 found on its own (88.7 % identical); without it, the menu is
  reflected on the ground and the reflected scene goes over it; with it, the menu is clean
  (`results/urti/bac_a_sable/interface/`). `ui_photo=0` to compare.
- 7 scenes equal or better (`suite/tableau_v37.txt`); popping 0.138 %.

## v38 — URTI: stable identities, budget respected, without OptiScaler

- Log of the v37 session in RDR2: budget without effect (8.37 million vertices reserved, ~200 draws refused per frame)
  because of **120,000 identities for ~800 objects on screen**: the identity included the instance count and the first
  instance, which change every frame (instanced vegetation). It now keeps only the mesh (index or vertex count, start
  in its buffers); for an ExecuteIndirect, its command signature; plus the rank. Sandbox: 381 measurements in memory.
- Budget respected: an unknown object enters the RT scene only while the budget allows; beyond that it is measured in
  the measurement area (within its per-frame limit). Saturation reproduced: 30,697 vertices for a budget of 30,000,
  0 draws refused.
- Snapshot before the UI: in RDR2 the rank of the right snapshot changes from frame to frame (the game records its
  commands on several threads). Chosen on the GPU in the same frame (`live_uiselect.hlsl`): a snapshot of this frame
  (more than 60 % identical to the final image and closer to it than to the previous displayed image, otherwise it is
  an old image), the least identical one (the earliest, with the least UI); copied into a buffer that reflections and
  composition read.
- "One frame in two": OptiScaler had frame generation on (`FGOutput = DLSSG`); URTI applied only to real frames. The
  user removed it.
- **Crash at startup without OptiScaler** (ucrtbase, 0xc0000409): ReShade, loaded directly (`dxgi.dll`), unloads and
  reloads add-ons for every device RDR2 creates at startup to test the card; the function-table entries replaced by
  URTI (`CreateRootSignature`, `ExecuteIndirect`) then pointed to the code of an unloaded add-on. They are now restored
  when the add-on unloads. Checked: the game starts (`tools/reshade/rdr2_demarrage.ps1`: click-free start, watches
  processes and crashes).

## v39 — URTI: NVIDIA denoisers (NRD / SIGMA) integrated; jitter, cause of the remaining shimmer

- **NRD 4.17.3** (NVIDIA, RTX SDK license, which allows integration into an application; sources and libraries
  outside the repository, in `urti/third_party/NRD`, built with NRI statically, DXIL shaders only, **without Agility
  SDK support**: otherwise NRI calls `ID3D12Device15::TryCreate…View`, missing from the system's DirectX 12
  (`ID3D12Device14`) → writes anywhere, crash in D3D12Core). CMake includes it if it is built (`URTI_NRD`).
- Integration (`urti_nrd.inl`, setting `nrd=1`): SIGMA (shadows) through the NRDIntegration layer on NRI, which wraps
  the game's D3D12 device, its command queue and our textures (classic barriers). Inputs: `live_shadow` in SIGMA mode
  (one ray per pixel, penumbra = occluder distance × tan of the sun's radius, and the weight of URTI's corrections),
  `live_nrdprep` (linear depth, normals encoded with `NRD_FrontEnd_PackNormalAndRoughness`), the game's motion vectors
  as they are (scaled according to their convention). NRD runs on its own command list (it installs its descriptor
  tables: nothing leaks into ReShade's state), submitted between our passes; `live_nrdpost` rebuilds the mask. A crash
  inside NRD is caught (symbolised call stack in the log) and URTI falls back to its own denoising; debug symbols are
  generated in Release.
- Measurements (sandbox): SIGMA is **worse** than URTI's denoising (tree in the wind, blinking 0.115 → 0.301 %; walk
  0.801 → 0.929 %); disocclusion threshold, number of frames, sun size: no effect. **Without sub-pixel jitter**
  (`URTI_SANDBOX_JITTER=0`), the tree in the wind becomes almost perfect with both (URTI 0.000 %, SIGMA 0.010 %): the
  remaining shimmer of a still scene comes from the game's **jitter** (DLSS / TAA). URTI computes on the jittered depth
  and composes on the already stabilised image; NRD requires that offset (`cameraJitter`), which URTI does not know.
  SIGMA off by default for now.
- Lead: the jitter shows up in the frame-to-frame transform measured by the cascades (live_sunvote) as a shear,
  separable from the rotation (`A[0][2] + A[2][0]`, `A[1][2] + A[2][1]`).

## v40 — URTI: the game's sub-pixel jitter and field of view measured and compensated

- The game's sub-pixel jitter (DLSS / TAA) is measured every frame without knowing anything about the game: the
  jittered projection adds a shear to camera space, x' = x + a w, y' = y + b w; the frame-to-frame transform measured by
  the cascades is A = K_prev S R S⁻¹ K_cur⁻¹. To first order, a_prev − a_cur = A[0][2] + s² A[2][0] (b with t²), where
  s, t are the stretch of URTI's camera space (assumed field of view) relative to the real one. s² and t² are measured
  by regression (decaying sums) on frames where the camera turns: without them, the rotation leaked into the
  measurement (~16 pixels of drift while walking). Absolute offset: sum of the differences minus its running mean (a
  game's sequence is centred); sandbox: ±0.4 pixel, the 8-frame Halton cycle recovered.
- **The game's field of view measured** (s = tan(true fov / 2) / tan(assumed fov / 2)): sandbox 54.9° to 55.1° for 55°
  (URTI assumed 50°). Logged; not yet fed back into the projection.
- Compensation (`jitter=2` by default; 1: the difference only, 0: nothing): the composition reads mask and reflections
  at the unjittered position (the presented image is stabilised by the TAA), reflections take their colour at the
  unjittered position, histories are reprojected with the jitter difference. Realigning only the history is worse (it
  then follows the jitter instead of averaging it).
- 7 scenes, all better (`suite/tableau_v40.txt`): blinking building 0.077 → 0.003 %, still tree 0.124 → 0.054 %, tree
  in the wind 0.115 → 0.050 %, character 0.076 → 0.034 %, fixed camera 0.098 → 0.081 %, occlusion 0.126 → 0.081 %,
  walk 0.801 → 0.803 %.
- `jitter_log=1`: jitter measured every frame in the log.

## v41 — URTI: ExecuteIndirect geometry reserve learned per signature

- First v40 session in RDR2 (launched by URTI): the game starts, field of view measured 49.5° to 53.8° (it varies:
  aiming, camera), 49 to 53 fps. New summary of the largest objects: **ExecuteIndirects reserve 65,535 vertices and
  write 0 to 10,000** (86289/86289 65535/2787 65535/10242 65535/0…): out of 8.3 million reserved vertices, 1.3 to 1.5
  million written — over 80 % of the slot is empty, hence its saturation (50 to 230 draws refused per frame) and the
  structure's cost.
- Learning per identity did not take: the order of ExecuteIndirects changes from frame to frame in RDR2. Size now
  learned **per command signature**: the largest number of vertices written by its draws (decays by 2 % per frame); a
  region filled to the end (truncated) doubles the learned size; reserve = learned size × 1.25 + 64. Sandbox unchanged;
  to be checked in RDR2.

## v42 — URTI: wet floor removed, field of view corrected on its own, ExecuteIndirects identified by their arguments

- In-game feedback: the wet floor added by URTI is buggy (RDR2 has no such look); reflections sometimes seem to "float
  above" the puddles. Wet floor off by default (`UrtiWet` = 0 in urti.fx; the setting stays for testing).
- **URTI's projection corrected with the measured field of view** (`fov_auto=1` by default): when the measurement is
  reliable (enough camera rotation) and differs by more than 3 %, p11 is corrected by 80 % of the difference, the
  measurement restarts from zero and the next 6 readings (three frames of latency) are ignored. Sandbox: 50.0° → 54.1° →
  54.9° (true: 55°), stable. In RDR2 the measured field varies from 49.5° to 60.6° (aiming, camera). With exact angles,
  the floor's normal and the reflection direction are no longer skewed. Suite: walk, deviation from truth 0.96 →
  0.90 %, added flicker +0.054 → +0.025 %; other scenes equal or better (`suite/tableau_v42.txt`).
- Log of the v41 session: the reserve was still full ("65536/22176 65536/65535 65536/0"): one command signature serves
  ExecuteIndirects of every size, and its maximum imposed 65,536 vertices on all of them. Identity of an
  ExecuteIndirect: signature, argument buffer and position in that buffer (no more rank); size learned per identity
  (0 included: reserve of 66 vertices); a region filled to the end goes back to the maximum reserve, and the next
  measurement gives the true size again.

## v43 — URTI: shadows remove only sunlight; the game's shadow map finally read correctly

- Request: a shadow is the sun's visibility; it must remove only direct light, not darken the whole colour
  (`c *= 1 - strength × (1 - mask)`, same proportion everywhere, even in overcast weather).
- **Sun share measured in the game's image** (`live_sunlight.hlsl`, on every present, before the composition): one
  point drawn per 4×4 block of render pixels, facing the sun (N·L > 0.2), closer than 100 m, clearly lit or clearly in
  shadow in the game's map (5×5 neighbourhood in the same state); a partner 10 pixels away on the same plane, in the
  other state. Ratio of the image's colours: shadowed / lit = sky / (sky + sun × N·L), hence x = sun / sky per channel
  and in luminance, accumulated in histograms (log2, quarter octaves) in the sun buffer (bytes 1232 to 1887), median
  read on the CPU, smoothed in log (5 % per frame). Fewer than 32 pairs: value kept.
- **Physical shadow**: the mask removes x N·L / (1 + x N·L) of the luminance (instead of the fixed slope
  min(1, 2.5 N·L)); the composition applies it per channel with the measured tint (the sun is warmer than the sky: the
  shadow turns blue like the game's). `UrtiStrength` = 1 by default: as dark as the game's shadows. `sun_measure=0`:
  old rule.
- **Bug found: the game's shadow map was read wrongly from the start.** RDR2 (and the sandbox) store it as R16 (format
  53, two bytes per texel); URTI read four bytes as a float: absurd values, reads off the rows. Affected: the mode
  "only the shadows the game lacks" (decided at random pixel by pixel, moving with the cascades: a likely cause of the
  wriggling shadows in game, where that mode is chosen) and the off-screen foliage-hole test. 16-bit read added (bit 32
  of `live_shadow`'s flags).
- **Combination with the game's shadow**: lit share filtered over 3×3 texels (its penumbra included) and final
  visibility = the lower of the two (URTI removes max(0, game − RT)), instead of multiplying by a game shadow widened by
  two texels (all or nothing). Default mode: "only the shadows the game lacks" (`UrtiMode` = 1): "full" mode removes the
  sun twice where the game already shadows. `game_min=0`, `game_dilate=`: old combination, for testing.
- Bug avoided: a `float3` in the composition's constants, moved to the next 16-byte boundary, received only red: three
  separate numbers.
- **New test** `tools/reshade/urti_sandbox_ombres.sh`: the "game's" map forgets poles, posts, wall, carts and
  passers-by (`URTI_SANDBOX_GAMESHADOW=partiel`); it is compared with the launch where the game shadows them (dry
  ground, reflections off). Forgotten shadow recovered, before → after: street 56 → 90 %, occlusion 57 → 91 %, walk
  55 → 68 %; darkness in the street (RT light / game, red green blue) 2.5 1.9 1.6 → 1.08 1.12 1.11 (right tint);
  deviation outside the area 1.9 / 1.3 / 0.5 → 0.15 / 0.11 / 0.19 % (no more double shadow); flicker in the area while
  walking 6.4 → 5.2 % (`ombres/tableau_avant.txt`, `ombres/tableau_v43.txt`). Sun / sky measured in the sandbox: 4.5
  (red 5.8, green 4.3, blue 3.0).
- Reflection suite (ground truth = game shadows only): deviation building 2.13 → 0.08 %, passers-by 1.60 → 0.47 %, walk
  0.90 → 0.60 % (jumping 1.10 → 0.50 %, blinking 0.81 → 0.57 %), occlusion 1.32 → 0.36 % (`suite/tableau_v43.txt`).
- Left: while walking, thin and distant shadows (poles, cables) are over-smoothed (68 %).

## v44 — URTI: RT reflections only on flat surfaces; RDR2's shadow map does not match

- In-game feedback (v43, a Saint Denis garden in the sun): reflections still shake, things jump, **trees have blurry
  grey dots that shake**, and shadows make **no difference** on or off. Log: sun / sky measured 0.06 to 0.16 (6 to 13 %
  of the light removed: invisible shadows) while the scene has sharp shadows; 50 to 115 objects out of ~800 in the RT
  scene; 48 fps, ~7 ms of URTI per frame. Player's preset at the end: "full RT shadows" mode, strength 1.
- **Grey dots**: RT reflections landed on every surface the game declares smooth (hair, leaves, skin); the depth
  normal there is noise, the reflection went anywhere and blinked (seen on the character's neck in a capture). The
  flatness check (until now reserved for the wet floor) now applies to every reflection: neighbours 2 pixels away (at
  4, in the distance they fell on another object: jumps while walking 0.50 → 0.91 %), and a surface is refused only if
  it has relief in both directions (the edge of a puddle against a wall stays flat). Sandbox (exactly reproducible:
  check off = v43 to the digit): jumping / blinking, walk 0.501 / 0.565 → 0.484 / 0.557 %, trees in the wind
  0.019 / 0.041 → 0.016 / 0.036 %, fixed street 0.066 / 0.075 → 0.057 / 0.063 %. The game keeps its own reflections on
  refused surfaces.
- **RDR2's shadow map does not match** what URTI expects (session launched by URTI in the bayou, overcast): check added
  in `live_sunlight` (points facing the sun that the map says lit / shadowed / on the edge / outside the cascades, mean
  luminance of the first two): **100 % on the edge**; the "recomputed game shadow" view is white everywhere. In the
  sandbox: 65 % lit, 31 % shadowed, luminance ratio 5.9. The depth test in RDR2's map could never be checked (wrong
  read until v43): the depth URTI's projection gives the point is probably not the one the game writes (viewport depth
  range, bias…). Added to the log every 300 frames: histogram of the points' depth in the cascade and of the map − point
  difference (sandbox: points within 0.375 to 0.625; zero difference for lit points, very negative for shadowed ones).
  To be recorded in RDR2 **in the sun** to fix it.
- **Safeguard**: the sun / sky measurement is taken only if the map agrees with the image (lit points at least 1.3
  times brighter than shadowed ones); otherwise the default value stays (2.2: 69 % of the light removed facing the sun,
  the old look) instead of erasing the shadows.
- Bug in the check tool: `show_mask=1` in urti.ini ticked "mask only" in the effect and never unticked it: white screen
  (mask without shadow) after the check. Now unticked when the line is removed. The game, launched by URTI for this
  check, was closed (no progress played).

## v45–v46 — URTI: measuring in the game (bursts); RDR2's RT scene was drawn at random every frame

- Request: "the reflections jump, you say it's fixed but it never is". Sandbox measurements are not enough: RDR2
  itself must be measured.
- **Burst** (F8 key or trigger file `urti_rafale`): 30 consecutive frames saved without URTI (copy of the image before
  the composition) and with it, in `urti_rafale_N\`. `tools/reshade/urti_rafale.py`: URTI's contribution C = after −
  before; what jumps = the change of C from one frame to the next (independent of the scene); shadow / reflection
  areas; objects entering / leaving the RT scene per frame (BURST lines of the log); map of the jumps and a close-up of
  the worst area. Results in `results/urti/jeu/rafales/`.
- RDR2's settings re-read (system.xml): RTX 4070, DX12, DLSS, HDR, motion blur off, sharpness at maximum, screen-space
  water reflections on (the game keeps its reflections where URTI puts none), several shadow sources (cascades, distant,
  terrain, screen space, long). Nothing that skews URTI.
- Session in RDR2 launched by URTI (Saint Denis at night, wet cobblestones): URTI changed 0.2 % of the pixels; at the
  biggest jump, the orange reflection of a street lamp appeared for two or three frames, then disappeared. Log: **0 known
  objects in the RT scene, 6.1 M vertices never measured, 183,499 measurements in memory for ~930 objects on screen**.
  Cause: the identity of an ExecuteIndirect (v42) included the address and position of its arguments, which rotate in
  RDR2: a new identity every frame, never measured, maximum reserve; the unknowns filled the budget in recording order
  (variable): the RT scene was a different draw every frame. The sandbox (fixed arguments) could not show it.
- **Stable identity**: command signature, pipeline, bound index and vertex buffers (events `bind_index_buffer` /
  `bind_vertex_buffers` tracked per command list), maximum command count. In RDR2: 671 objects on screen, **671 in the RT
  scene, 0 never measured, 785 measurements in memory, 0 out of budget**, 0.4 objects entering or leaving per frame.
  Fixed-camera burst, URTI's reflection area: shaking 8.2 → 1.5 %, jumping 0.88 → 0.12 %. Budget raised to 6 M vertices
  (a slot holds 8 M). Sandbox unchanged to the digit.
- Cost: the RT scene now holds real triangles (4.5 M vertices): structure build 2.6 → 4.1 ms, reflections 0.2 → 1.0 ms;
  ~51 fps in this night scene (64 before, with a meaningless RT scene). The structure is rebuilt every frame (300 out of
  300): an update requires the same layout, but each object's place in the stream follows the draws' recording order,
  which changes from frame to frame (RDR2 records on several threads). To do: a fixed place per object.
- Not checkable that night: daytime reflections, shadows (no sun). A burst with a moving camera mixes the normal motion
  of the reflection with jumps: to refine.

## v47 — URTI: F4, full-screen before / after

- Player's request: a button to see the difference live. **F4** switches between the game's image alone and the image
  with URTI, full screen and instantly: only laying the effect on the image is skipped, the RT computation goes on (so
  the frame rate does not change: for that, F7 turns everything off). F5 stays the split screen (left without, right
  with).

## v48 — URTI: "URTI on" checkbox in the menu (all computation off), F7 in sync

- Request: a button that completely turns ray tracing off (no computation at all) and back on. `UrtiOn` checkbox in
  the ReShade menu: unticked, no more geometry capture, structure, shadows, reflections or composition; ticked again,
  everything resumes. F7 does the same and the box follows. No effect if urti.ini lacks `live=1` (native sandbox
  launches stay native; suite unchanged).
- "No difference" with F4: the player's game was at night (sun tracked on 0 % of frames, no sun shadow map). At night,
  URTI makes no shadows (only the sun's) and its reflections, limited to flat surfaces, reflect a dark scene: 0.2 % of
  pixels changed (burst). The reflections visible on wet cobblestones at night are the game's.

## v49 — URTI: no more menus in reflections; no more invisible shadows costing 4 ms

- In-game feedback (Saint Denis, rainy day, ground wet by the game): reflections shake, **menus show up in the
  reflections on the ground**, reflections come off the ground, no difference for shadows; 20 to 27 fps. Log: sun
  tracked, matching shadow map (lit points 3.2 times brighter than shadowed ones), but sun / sky measured 0.06 (overcast:
  6 % of the light removed); GPU: shadows 3.8 ms, reflections 3.1 ms, sun and structure 3.9 ms. "No snapshot before
  the UI" 27 times in the session.
- **Menus in reflections**: the snapshot before the UI (the reflections' colours) was kept only if it looked more like
  the current image than the previous one. Still image (the player looks at the ground, a menu is open): a tie, no
  snapshot, colours read from the final image, menus included. The GPU now keeps its choice from frame to frame
  (`live_uiselect`, bytes 64 / 68 of its counters): the previous snapshot stays if it still resembles the final image
  by at least 30 % (a large menu covers part of it) and is not a copy of the previous displayed image (snapshot taken
  too early). If no snapshot is safe, RT reflections fade out smoothly (−15 % per frame) and come back with it. Sandbox,
  still shot without sub-pixel jitter, menu open 2 s every 4 s: snapshot lost 2 times out of 600 frames → 0.
- **Invisible shadows**: sun / sky measured under 0.15 (less than 13 % of the light removed: overcast, dusk): no more
  shadow rays (3.8 ms saved in this scene); they resume when direct light comes back (the measurement goes on).
- Reflection suite unchanged (`suite/tableau_v49.txt`). Not checked in game: to measure (F8 bursts on the wet ground,
  and one with a menu open).

## v50–v52 — URTI: a single checkbox, RT ambient occlusion, sun shadows off (measured: invisible)

- Player feedback: "0 difference" and many frames per second lost; too many options; asks for RT "the NVIDIA way" (one
  checkbox, everything automatic), ambient occlusion, even path tracing and Ray Reconstruction; a fine-weather option;
  a crash when opening the Social Club (Home key, "memory error").
- Analysis of the session: **reflection strength at 0 in their menu** (`UrtiReflect=0`) and shadows suspended
  (overcast): URTI could change nothing, while costing 4 to 5 ms (structure, sun tracking).
- Research: the "ray tracing" seen on RDR2 is made of screen-space ReShade shaders (Pascal Gilcher's RTGI / PTGI,
  without a sun direction); Ray Reconstruction can be injected (experimental OptiScaler forks) only into games that
  already have path tracing; fine weather on demand: ScriptHookRDR2 and a trainer (third-party files, not installed
  without agreement).
- Social Club crash: not reproduced (Home key sent: no menu, no crash); no crash in the Windows log since 10/31;
  ReShade opens on Page Down (no conflict); video memory 7.0 / 12 GB. To revisit with the exact error text.
- **Menu reduced to "URTI on"**: the other settings are hidden and ignored (the preset's old `UrtiReflect=0` no longer
  turns anything off): reflections at 1, shadows "only those the game lacks", no wet floor; urti.ini still rules for
  tests.
- **RT ambient occlusion** (`live_shadow`, ao=0.8, ao_radius=1.5 m): two short rays per pixel (cosine-weighted
  hemisphere, rotating pattern), occluder at t: occlusion (1 − t / range)²; foliage holes recognised; removes only sky
  light (all of it in shadow, 1 / (1 + x N·L) in the sun, x = measured sun / sky); denoised with the mask. **Only on flat
  surfaces** (ground, walls, roads) and closer than 50 m: on grass and fences every blade is geometry, everything
  darkened (the game already has its occlusion there) and shook.
- **RT sun shadows off by default** (`shadows=1` to turn them back on; flag 64: the sun stays known for occlusion):
  burst in the sun without occlusion: **0.0 % of the image changed for 3.2 ms**. RDR2's shadows leave almost none to
  add.
- Denoising of the mask when it carries only occlusion: more accumulated frames (current share 0.05 to 0.3) and a wider
  clamp (2 standard deviations).
- Measurements in RDR2 (bursts, `results/urti/jeu/rafales/`):
  - v50 (occlusion everywhere): 32.5 % of the image changed, jumping pixels 0.21 %, shadows + occlusion 6.1 ms;
  - v50 flat occlusion: 20.2 %, 0.12 %, 5.1 ms;
  - v51 (no sun shadows, fog): 8.2 %, 0.08 %, 1.2 ms, 56-58 fps;
  - v52 (bayou in the sun): 9.4 %, 0.05 %, darkened area shakes 1.2 % (4.1 % in v51), 2.3 ms, 44 fps.
- Left:
  - one-frame flashes when objects enter or leave the RT scene (frames 6, 11-12, 15 of the v52 burst: 4 to 10 objects;
    the game skips some objects for a frame);
  - shaking reflections on water (reflection area: 17 % shake);
  - the structure rebuilt every frame (3.8 to 5.7 ms);
  - the snapshot before the UI still lost at times in RDR2 (snapshot resemblance now logged).

## v53–v55 — URTI: a fixed place per object: no more flashes, structure updated instead of rebuilt

- Player's opinion: leave shadows to the game (its own are too good); step 1: stability and speed.
- Cause of the one-frame flashes (v52 burst): 8 to 9 objects per frame entered or left the RT scene, with no refusal
  and no budget overflow: the game does not draw everything every frame (vegetation, levels of detail, culling). And
  each object's place in the stream followed the draws' recording order, which varies: the structure was rebuilt 300
  times out of 300 (3.8 to 5.7 ms).
- **Fixed places** (`g_livePlaces`, urti.ini `persist=0` for the old rule): each object of the main screen keeps its
  place from frame to frame (first free space large enough, a quarter of margin, free spaces merged); an object the
  game skips for at most 3 frames keeps its last geometry in the RT scene; beyond that, its place is released. The
  counts of written vertices are copied into one cell per place (`live_counts.hlsl`), which the conversion reads; the
  conversion covers all held places (table sorted by start). A refused place, or more than 70 % of the slot used:
  everything is redistributed on the next frame.
- Missing triangles: flat (vertices at the origin) instead of NaN: an update of the structure cannot change the
  active / inactive state of a triangle. **Update as long as the converted part keeps its size** (full rebuild at most
  every 8 frames).
- v53: the used part filled the whole slot (82 draws refused per frame, 406 objects in the RT scene): hence the
  margin, merging and redistribution of v54.
- Measurements in RDR2 (v54, Saint Denis in the sun): 871 places held (71 MB), used part 81 MB out of 128, 0 new places
  per frame, 0 redistributions; **33 rebuilds and 267 updates out of 300**; "sun and depth" step 1.3 ms (3.8 to 5.7);
  **0 objects entering or leaving over the 30 frames of the burst**, 0.015 % of pixels jumping; 65 fps. Sandbox: suite
  identical to the digit with or without fixed places.
- Ambient occlusion in motion (sandbox, walk): jumping 0.47 % without it, 0.78 % with it; denoising of the
  occlusion-only mode widened (5×5 blur guided by depth, current-frame share in motion 0.2): 0.69 % (blinking 0.84 →
  0.71 %). Still to reduce.

## v56–v66 — URTI: decals, snapshot before the UI impossible in RDR2, structure updates abandoned

All measurements below are taken in RDR2, launched by URTI; scenes differ from one launch to the next (the save reloads
elsewhere): only comparisons within one launch are valid.

- **Occlusion in motion**: 4 rays instead of 2 where the image moves by more than a pixel and a half (motion vectors);
  foliage excluded from receiving surfaces (neighbouring points 6 pixels away seen clearly further: the background
  through the leaves).
- **Reflections on water**: normal from a centred difference 4 pixels on each side when both sides are on the same
  surface (ripples smoothed); no reflection ray below 3 % weight (Fresnel at normal incidence), range 500 m.
- **v56: extra screen snapshots (1st, 2nd, 8th, 32nd draws): RDR2 crashes in its menu** (fatal "memory" error),
  removed (v57).
- **Dark straight-edged spots on the ground** (seen by the player): the game's decals (mud, marks, puddles), laid just
  above the road without writing depth, stopped occlusion rays a few centimetres away. Occluders closer than 6 cm
  ignored (5 cm for reflections): spot gone (mean darkening of the ground 3.5 → 0.1 level).
- **Snapshot before the UI**: burst extended (raw snapshot, kept snapshot). In RDR2, **the screen receives a single draw
  per frame**, which assembles the scene and the UI prepared elsewhere: the snapshot taken before it is a previous
  frame, UI included (minimap identical to the final image); no snapshot without UI exists. Worse: the "kept" snapshot
  (v58) resembled the current image by only 2 %. Fixes:
  - in RDR2, `ui_photo=0` (the game's urti.ini);
  - never a kept snapshot again;
  - UI detected with a tolerance (the game's grain of one or two levels);
  - **fixed UI zone** `ui_zone=0.012,0.69,0.16,0.97` (minimap and icons): minimap altered 7.5 → 1.7 levels.
  - The snapshot taken right after the first draw (only in a direct list) no longer crashes the game.
- Motion-vector convention re-measured in game: the same (no. 2). In motion, the effect follows the image at **98 %**
  (`tools/reshade/urti_rafale_decalage.py`: shift of the effect / shift of the image by phase correlation).
- **Sub-pixel jitter**: compensation off in RDR2 (`jitter=0`): shaking 0.19 → 0.13 %; reflection area shakes 2.57 →
  1.79 %, jumps 0.10 → 0.06 %. The sandbox keeps the compensation.
- **Structure updates**:
  - v58: places redistributed every frame (70 % threshold exceeded) while updates were still allowed: 4.5 fps. Fixed
    (rebuild required after a redistribution).
  - Flat triangles at the origin (the camera): an update that made them real stretched boxes across the whole view
    (rays 10 times slower); now laid on the first vertex of their place.
  - Regulator based on measured cost (rebuild against slower rays).
  - Final measurement, same scene: updates 1.2 + 3.7 + 6.8 ms (structure, occlusion, reflections), 38 fps; rebuild
    every frame 5.7 + 1.2 + 0.5 ms, 45 fps. A single update makes reflections fourteen times slower (vegetation batches
    sorted by the GPU: triangles change places).
  - **Rebuild every frame by default** (`blas_refit=0`); places without margin, redistributed as soon as a fifth of the
    used part is lost to holes.
  - Rail yard, overcast: URTI 3.9 ms in total, 73 fps.
- Player's opinion of v66: "perfect, the look is insane"; frames per second still to gain.
- **Sandbox** (`suite/tableau_v66.txt`):
  - trees and character clearly better (jumping 0.010 → 0 %);
  - **moving camera worse**: jumping 0.63 → 1.24 % (blinking 0.68 → 0.55 %), larger deviations from truth everywhere.
    To understand: reflection changes (weight threshold, minimum range, centred normal) or the snapshot before the UI
    (snapshot after the first draw, tolerance).

## v67 — URTI: optimisation (geometry budget driven by measured cost, quarter-rate occlusion, lighter composition)

- Request: frames per second. Costs measured in RDR2 (v65-v66): rebuilding the RT scene dominates (1.8 ms in the rail
  yard, 5.7 to 8.8 ms in a rainy street); occlusion ~1 ms, reflections 0.2-0.5 ms, composition 0.8 ms.
- **Geometry budget driven by the measured rebuild cost** (`build_ms=2.5`, 0: fixed budget): every 30 frames, −15 % if
  the rebuild exceeds the target by 10 %, +8 % under 75 % of the target, between 1.5 M vertices and `vertex_budget`
  (6 M). Objects with the smallest apparent size (box / distance) leave first.
- **Quarter-rate occlusion** (`ao_quarter=1`): one pixel of each 2×2 block casts its rays each frame (all four in four
  frames), twice as many rays per pixel (4 at rest, 6 in motion); the others are written −1 and rebuilt by the
  denoiser (5×5 blur over computed pixels only, history alone without a computed neighbour). Sandbox: occlusion pass
  0.86 → 0.39 ms, quality measurements unchanged.
- **Composition**: mask read bilinearly (4 reads instead of 16) when it carries only occlusion: 0.51 → 0.46 ms
  (sandbox).
- **Sandbox regression with a moving camera explained** (jumping 0.63 → 1.25 % since v62): occlusion off, 0.48 % (the
  earlier level). The ground truth has no occlusion: darkening that follows the scenery counts as jumps there. Before
  v62, the exact UI comparison prevented occlusion on a large share of pixels (weaker effect). In game, the effect
  follows motion at 98 % (`urti_rafale_decalage.py`).
- For the player to check in RDR2: frames per second, cost per step (log), missing objects when the budget drops.

## v68 — URTI: optimisation validated in RDR2; aberrant vertices (RT scene invisible since v66)

- v67 in RDR2: occlusion and reflections had no effect at all (0.0 % of the image, white mask, pass at 0.13 ms). Rays
  hit nothing. Culprit: the combination of fixed places + rebuild every frame (v66); with fixed places off, or updates
  allowed, everything comes back. Cause: aberrant vertices (non-finite or huge, from kept or never-written places); a
  full rebuild stretches the whole scene's box, and the inner boxes, stored with a precision relative to it, become
  worthless. The conversion now folds any non-finite vertex, or one further than 20 km, onto the first vertex of its
  region. The v66 that the player judged "perfect" therefore ran without RT occlusion in some scenes.
- Optimisation measured in game (same session, Saint Denis, twice in a row): full occlusion 3.75 / 3.88 ms,
  54.8 / 52.9 fps; **quarter rate 1.99 / 2.16 ms, 59.1 / 57.3 fps**. Burst: 0.001 % of pixels jump. Rebuild 2.25 ms
  (under the 2.5 ms target: budget unchanged here).

## v69 — URTI: RDR2's G-buffer decoded, G-buffer choice fixed, surface cache, occlusion tuned on its time

- **RDR2's G-buffer** (pass survey, raw `.raw` texels of every channel, `tools/reshade/urti_gbuffer.py`): target 0 =
  colour (albedo, RGBA8 sRGB; alpha: 8 values, a material marker); target 1 = world normal (z up, encoded 0..1, length
  1.00), alpha = character marker; target 2 = material: r type (0.5 common, 0.2 foliage, 0.31 skin), **g smoothness**
  (puddle 0.60, brass 0.49, skin 0.53, shirt 0.03), **b metalness** (brass balls 0.80, iron poles 0.57, fences 0.44,
  elsewhere 0), a occlusion or wetness (rewritten later in the frame by another pass); target 3 = r almost 1 everywhere,
  b subsurface scattering (skin 0.53); target 4 = motion vectors (RG16F).
- **Defect found**: the G-buffer was the last binding of three or more targets on the screen depth. A later pass
  (transparents: 087, 088, 089 + two from the G-buffer) replaced it at times, and the G-buffer itself is bound with 3
  targets for part of the objects (colour, normal, motion): "target 2" was then the motion vectors, read as smoothness.
  Now: the set of targets the game draws into the most, reviewed every 30 frames, keeping the widest binding for the
  same first target. Checked in RDR2: `5 targets (formats 29 28 28 28 34)`, stable.
- **Surface cache** (`live_surfcache.hlsl`): one pixel per 4×4 block per frame (the whole screen in 16 frames), a ray
  from the camera to the triangle seen, the image's colour kept per triangle (RGB9E5 + object identity, 8 bytes, 21 MB
  for 2.8 million triangles). A reflection whose hit point is hidden or off screen takes that colour instead of staying
  unknown. Sandbox: window reflections extended at the edge of the screen, deviation from truth unchanged (2.59 %,
  almost everything there is visible on screen). RDR2: works, +0.7 ms, small visible gain in the scenes seen (RT
  reflections on 2 % of the image).
- **Finding in RDR2**: the floor of Saint Denis's arcades has smoothness 0.36 (polished stone, blurred reflection),
  under the RT reflection threshold (0.5): RT reflections cover only 2 % of the image. Blurred reflections (rays drawn
  according to roughness + denoising) are a separate step.
- **Occlusion: 5.5 ms under the arcades** (68 fps without it, 48 with it). The cost depends neither on the range (0.2 m
  as 1.5 m), nor on opaque rays or the hole test, nor on the scene's size (3 times fewer vertices: the same); it doubles
  with the number of rays. Under arcades, almost the whole screen is a close flat surface (outdoors, sky and distance
  are skipped). The number of rays per computed pixel is now tuned every 30 frames on the pass's time (target 2.5 ms,
  `ao_ms=`, from 4 to 2 rays).
- Table of the RT scene's regions copied into video memory every frame (conversion, reflections and cache looked up
  their region across the PCIe bus).
- Tools: `urti_rdr2_rapide.ps1` no longer takes the old log or "first frame" for a running game, recognises the menu
  without a calibration capture, does not click when someone is using the PC.
- To check in RDR2: tuning of occlusion rays and the frame-rate gain under the arcades; cost of the cache with the table
  in video memory.

## v70 — URTI: light bounce (indirect lighting); surface cache finally active in RDR2; minimap protected

- **v69 fix**: in RDR2, the surface cache was never read. The test "the cache covers the scene" (triangles × 3 ≥
  capacity) failed by 2 vertices: the capacity (8,388,608) is not a multiple of 3 (in the sandbox it is). The
  difference measured in v69 (0.4 % of pixels) was noise. Now: triangles ≥ capacity / 3.
- **Cache per group of eight triangles**: in RDR2, most triangles are smaller than a pixel; one pixel sampled per 4×4
  block almost never fell on the triangle a ray later hits (one occlusion ray in four found an entry, almost none of the
  right object). An entry covers eight consecutive triangles (neighbours in the mesh); empty entry or one from another
  object: the neighbouring entries of the same object.
- **Light bounce** (`gi=`, 1 by default): occlusion rays that hit an occluder take the colour the cache holds for it
  (the light the surface sends back), with the same weight as the occlusion (the sky light removed in that direction is
  replaced by the occluder's). Three planes R, G, B at quarter rate, denoised by the same filter as the occlusion
  (extended to three planes, same weights), received according to the surface's own colour (G-buffer albedo, copied
  every frame). No extra ray. Sandbox: lit sand sending warm light at the foot of walls and under barrels, flicker
  unchanged; +0.4 ms. RDR2: +0.85 ms (61 → 58 fps on the quay).
- **Characters excluded from the bounce**: alpha of the G-buffer normal (target 1, copied) = character marker (0.41 on
  Arthur, 0 on the floor). They move, their history is rejected every frame and the bounce, noisier than the occlusion,
  shook on them (burst: 0.20 % of pixels shaking, all on the character).
- **Minimap never protected since the fixed UI zone**: in the composition, `float4 uiZone` after 18 numbers was placed
  by HLSL at the next 16-byte boundary, 8 bytes further than what the add-on writes (the last two numbers outside the
  constants). Same defect in the surface cache. Four separate numbers; checked across all shaders (no other).
- Hot reload for the surface cache and the denoiser too.
- **RDR2 measurements** (wet quay in Saint Denis, bursts in steady state): without bounce, effect on 10.2 % of pixels
  (RT reflections of the sky on the quay: 9.7 %), shaking 0.01 %, jumping 0.000 %; with bounce, effect 10.8 %, shaking
  0.01 %, jumping 0.001 %.
- Measurement pitfalls: a burst taken a few seconds after reloading `urti.ini` shows reflections missing at times
  (state reset, to examine): wait 20 s. A diagnostic shader hot-reloaded stays active after its `.cso` is deleted
  (until the next reload or relaunch): a first "perfect" burst had been taken with a grey diagnostic bounce.

## v71 — URTI in a second game

First test outside RDR2, on a second DX12 game, without adapting anything first: ReShade 6.8 (dxgi.dll) and the add-on
placed in the game folder (no game file replaced).

- **What works on its own**: loading with Streamline (DLSS); geometry capture (1,600 draws captured out of 5,800, 3
  million vertices in the RT scene); depth law and projection; visible RT occlusion (under floors, at the foot of
  posts); G-buffer found (7 targets, native 2560×1440 rendering).
- **Cost too high**: 9 ms. The game's objects are heavy (up to 464,000 vertices, 13 million on screen against ~2.5 in
  RDR2): rebuilding the structure takes 3 to 11 ms, the budget drops to the floor (1.5 M vertices), only 46 draws in the
  RT scene and many entries and exits per frame. Native 1440p image (no upscaling): occlusion and composition more than
  twice as expensive as in RDR2 (1707×960).
- **A different G-buffer layout**: colour in target 0, octahedral normal in target 2 (RG16 UNORM), motion vectors in
  target 5. RDR2's settings read the wrong channels there: smoothness taken from the normal, character marker in a
  target without alpha.
- **Fixes**: motion vectors: with several RG16F targets, the first was taken (almost empty here: every convention tied);
  after three measurements without a winner, the next one is tried (kept in `urti_calib.ini`, `motion_target=`).
  Character marker configurable (`person_mask=0`: none). Pass survey: at most two snapshots per frame (an image
  rewritten at each step took every slot before the G-buffer), 64 in total.
- **Frame generation**: one present in six arrived without a counted draw and some calibration captures got 0 draws:
  the game probably records its command lists ahead, across two presents (to examine: URTI takes those frames for
  generated ones and keeps the previous RT image there).
- To do: per-game material channels; a geometry budget suited to heavy objects (per-object structures kept from frame
  to frame rather than a single one rebuilt); counting draws per present.

## v72 — URTI: capture ordered by execution; frame generation (DLSS-G) recognised

Towards a URTI that discovers how each game works instead of assuming it.

- **Diagnosis** (a counter per command list: present of its first draw, present of its execution): the second game
  writes **35 % of its draws before the previous present** (lists of the next frame prepared ahead); RDR2: 0 %. Ordered
  by writing, the draws mixed two frames: rank among draws with the same signature (hence identity) different every
  frame, regions read before being filled, counters taken over by other draws. Hence objects entering and leaving the
  RT scene (60 to 130 per frame), 80 to 110 new places per frame, an RT scene swollen with holes (4.2 M vertices for a
  1.5 M budget, rebuild 8.7 ms), calibration captures with 0 draws, and one present in six taken for a generated frame.
- **Capture in execution order** (ReShade's `execute_command_list` event). At writing time, a captured draw only
  receives a region in the part of the stream of its writing window (three rotating parts per slot) and a counter of
  that part. When its list executes: rank, identity, place in the RT scene and the present it belongs to. The
  conversion reads each place from its region of the stream (places now live in the RT scene's space, no longer in the
  stream); a place kept without being drawn is left as it is; outside any place: a flat triangle at the origin.
  Counters of the next part are reset at the end of each frame (before any list is written in the new window). A list
  executed more than two windows after its writing: ignored (never seen). Geometry budget per mesh (all its copies): a
  copy's identity is only known at execution, while the choice to capture is made at writing.
- **Frame generation** (DLSS-G): ReShade sits under Streamline: it sees two presents per game frame, on the same queue
  (the game executes its draws on another). Some games never draw into the presented image (Streamline's intermediate
  image), RDR2 does. Rules:
  - a present without any executed draw does not advance the capture window;
  - generated frame = a frame where the game did not draw; otherwise, when presents alternate, the one that follows
    the game's draws, because generation by interpolation shows the generated frame before the real one (URTI had them
    swapped: the blend landed on the real frame);
  - the RT computation runs as soon as the draws have executed; the generated frame receives the blend of the
    second-to-last and last results, the real one its own result;
  - the light bounce is kept on generated frames (off every other frame: blinking).
  The "generated first" order is deduced from how interpolation works, not yet checked in motion.
- **Measurements in the second game** (same place, frame generation on): shaking 1.91 % → 0.04 %, jumping 0.148 % →
  0.002 %; new places 80-111 → 9-15 per frame; refused 100-460 → 6-11; RT scene 4.2 M → 2.0 M vertices; cost 9 → 6 ms
  per real frame. But the effect drops to 0.5 % of pixels: 609 meshes and 14 M vertices on screen for a budget at its
  1.5 M floor. Next: per-object structures kept from frame to frame in a stable space (camera motion measured on our
  own geometry) instead of a rebuild every frame.
- **RDR2, non-regression check** (wet quay in Saint Denis): effect 10.2 %, shaking 0.01 %, jumping 0.000 % (as in v70);
  0 late draws, 518 stable objects (0 new places, 0 entries or exits per frame), 64.6 fps.
- Tools: `premier_plan.ps1` (is the game in the foreground, without reading anything else).

## v73 — URTI: camera motion measured on our geometry; crash on HDR switch / Alt+Tab; rendered area (DLSS, dynamic resolution); NaN depth law

- **Camera motion without the game** (the "motion" module of self-discovery). The conversion records pairs of
  vertices: position in the previous frame and in this one, for objects drawn at the same place in both frames. It
  takes about fifteen per object, so that a character following the camera weighs as much as a rock. Only direct draws
  of a single copy count, whose mesh is drawn only once in both frames: some games sort their draws, and the n-th copy
  of a mesh is not the same object from one frame to the next. `live_camfit` does the rigid fit (Horn's method),
  robust, repeated five times while discarding what moves; it restarts from stillness if the camera changes pace.
  `live_mvsynth` builds motion vectors from that motion and the depth (convention 0).
  - Automatic switch when the game's own stay unreadable: two targets tried without a winner, or 3 s of moving camera
    (our measurement) without a convention found. Setting `motion_source=`: 0 automatic, 1 game, 2 ours. Kept in
    `urti_calib.ini` (`motion_source=camera`).
  - Sandbox (walking camera, known truth): translation measured at 2.7 cm per frame, exactly the script's walk.
    Accumulation as good as with the game's vectors: added flicker +0.045 % against +0.046 %, jumping points 0.514 %
    against 0.522 %.
- **Crash when switching to HDR or on Alt+Tab** (reported by the user): the recreated screen image changes format
  (RGBA8 → RGB10A2). The snapshots before the UI were copied with the old layout, hence an invalid copy, a refused
  command list (E_INVALIDARG) and the crash. The list of screen images also kept addresses of destroyed images.
  - Fixed: layout taken again at every change of format or size, copy skipped if it does not match, list of screen
    images rebuilt every frame.
- **Rendered area**: with DLSS and dynamic resolution, a game may draw 1280×720 to 1704×960 inside 2560×1440 targets.
  URTI read the whole target: shadows and reflections stretched by 1.5.
  - The rendered area is now the largest of the frequent viewports of the screen's draws (full-size 3D markers and
    half-resolution passes discarded). It is counted at execution and changes only after ten frames in a row. Images
    stay allocated at full size. Test setting: `render_area=0`.
- **NaN depth law** in `urti_calib.ini`, written when the motion vectors switched. Read back at launch, it made every
  point invalid, and sending the sun marked it valid: no more occlusion or reflection (all-white diagnostic view).
  - Fixed: never written, never read back, never adopted if it is not finite; sending the sun no longer marks it valid.
- **Water reflections**: the water surface is not in the depth URTI reads. Water is drawn afterwards, transparent:
  URTI saw the bottom through it. Next step: find the layer where the game writes the water surface ("transparent
  surfaces" module).

## v74 — URTI: water reflections (layer of flat transparent surfaces); effect disappearing every five seconds

- **Effect that disappeared and came back every few seconds** (reported by the user): without a recognised sun shadow
  map, the full calibration failed and restarted every 480 frames; the RT scene capture stopped meanwhile. It is no
  longer restarted when RT sun shadows are off (the default) and the depth law is known (also measured every frame).
- **HDR screenshots**: Windows screenshots clip an HDR image; the burst's raw values, decoded (PQ), are the reference.
  The depth law measured every frame is now written in the log.
- **Water reflections** ("transparent surfaces" module of self-discovery). Some games draw their water at half
  resolution, in their own images and their own depth. It is never in the main depth: URTI saw the bottom.
  - Separate capture (layer 2 of the RT scene, mask 2) of two kinds of draws:
    - on the main depth, those that test depth without writing it, outside the chosen G-buffer;
    - all those of another view of the camera (same shape as the screen, reduced resolution).
  - Conversion into their own buffer (1.5 M vertices at most); those regions are addressed across the whole stream.
  - `live_planes` keeps flat surfaces (aligned normals, vertices close to the mean plane, more than a metre, not
    turned toward the camera like a particle) and makes the rest invisible to rays (NaN): smoke, particles, light
    volumes, sky.
  - The camera ray of reflections looks for this layer in front of the depth point: the reflection then starts from the
    water surface, with its triangle's normal, smooth.
  - All other rays (occlusion, shadows, cache, reflections) only see layer 1.
  - A perfect-mirror test (`reflect_f0=1`): the water sends back the trees. But the reflection comes in large blocks,
    and spots appear on the bank (ground at water level).

- `water=0`: no transparent layer.
- The "blended into the image" criterion was not enough: some water computes its refraction itself and writes its
  colour in one block. Measured in game: about 1,300 "depth tested, not written" draws per frame (the main pass redraws
  the scene into the G-buffer after a depth pre-pass), 110 of them blended; no tessellation.
- Not yet checked: RDR2 with the transparent layer (not reinstalled; RDR2 keeps the version checked in v72), the blocks
  of the water reflection.

## v75 — URTI: geometry cache (a whole forest in the RT scene); water reflection from the game's mask; causes of "effect / no effect"

Tests done in the second game, by a lake at night (haze, DLSS + DLSS-G + HDR).

- **Wrong depth chosen forever (v74 bug)**: the draws of other views went to "transparent" and no longer counted when
  choosing the main screen; a wrong depth chosen right after the menu stayed. The RT scene held 15 objects instead of
  the forest. Chosen on a running average now, every draw counted.
- **Capture counters cleared (cause of random empty objects)**: resetting the next window's counters ran at the end of
  the computation, on the present queue, while the game was already drawing on its own. Reset one window ahead, four
  parts of the stream instead of three. Surface layer: 13 % → 97 % of complete windows. GPU barrier added between the
  game's queue and the present queue (`queue_sync=0` to turn it off).
- **Geometry cache**: the per-frame budget held only 37 meshes out of 535 (14 M vertices on screen; rebuilding 2.5 M
  costs 5 ms). Each object outside the budget, captured in turn (500,000 vertices per frame), now gets its own
  structure, kept in a 1 GB reserve (ring buffer) and placed in the top-level structure with the camera's pose. About
  1,000 objects and 3 to 4 M triangles in the scene, 250 MB. `geo=0` to turn it off, `geo_mb=` for the reserve.
- **Camera pose without drift**: accumulating motion frame by frame drifted up to 0.9 m with a still camera (each frame
  without a measurement lost a piece of the game's sub-pixel jitter). Witnesses (small still objects, further than
  4 m, captured every frame) keep sixteen reference points in the cache's frame (`live_refpairs`); the pose is realigned
  on them. Still: stable within 3 cm; 96 witnesses, about 900 pairs per frame, motion measured on 290 frames out of 300.
- **Water reflection from the game's mask**: the "transparent layer" (v74) and "flat sheet" leads were wrong: the flat
  surfaces found were terrain tiles, haze cards (the large dark rectangles seen on screen) and slabs under the
  terrain. A census of all draws shows there is no separate water draw; the pass survey shows that one G-buffer target,
  at its last write (in compute), is exactly 0 on water, grass outlines included. The material copy happens at the end
  of that last compute write, otherwise the next frame's G-buffer copy overwrites it. Transparent layer off by default
  (`water=1` to bring it back): it cost time and made the false reflections.
- **Reflection accepted in two more cases**: nothing hit while the screen shows distance (more than 20 m: forest drowned
  in haze); hit point behind another distant object (foliage in depth: same material, same haze). The game alone makes
  pixelated green blocks here; with URTI, a continuous mirror.
- **Causes of "effect / no effect"**: (1) the reflection was cut on every raw generated frame, one present in two;
  (2) the depth law measured every frame was wrong (0.08 to 0.13 instead of 0.19: it averages vertices that do not all
  follow the same law), and the reflection dropped across the whole lake for series of frames: a known law now takes
  precedence (`depth_law_fixed=`); (3) the mask was cut after 2 presents without a computation, the bounce after 1 (8
  now); (4) the budget fell from 105 to 27 objects when a window contained two game frames (increase limited to 25 % per
  window).

Measurements, second game, three bursts, still camera: effect on 23 to 26 % of pixels (shadows 18 to 20 %, reflections
5 to 7 %), shaking 3.8 to 6.8 %, jumping 0.3 to 1.3 % (still far from RDR2: forest in the wind, moving character, haze);
300 presents out of 300 with the effect. Real Alt+Tab, back and forth, twice: no crash. GPU per real frame: cache
structure 2.3 ms, frame structure and sun 2.9, shadows 5.3, reflections 0.9, composition 1.6, i.e. 13 ms.

RDR2 (story mode, quay at dusk, different light from the 10.2 % measurement of v72, so the effect is not comparable):
effect 0.9 %, shaking 0.09 to 0.13 %, jumping 0.04 to 0.06 %, 63.7 frames per second (64.6 in v72), identical with
`geo=0`, no artefact seen. Add-on installed, old one kept as `urti.addon64.avant_v75`.

Tools: bursts while turning, real-pace captures (a burst freezes the game), Alt+Tab tests; `urti.ini` always restored;
stop without clicking if Steam Cloud waits for an answer; `alt_tab.ps1`. Diagnostics: `motion_log=1` (motion, layer,
draw census, flatness of objects), `reflect_diag=`, `trace_draw=`, view 5: red = hidden reflected point.

## v76 — URTI: the game's camera read from its memory instead of being deduced from geometry

In-game feedback after v75: "everything jumps in every direction, effect / no effect, it shakes everywhere". The v75
tests had only seen a still camera or one turning in a burst; the log of the player's session showed what happens when
moving:

- **Motion measured on the witnesses**: valid on 91 to 188 frames out of 300, off by 460 to 870 mm, 46 to 70 % of pairs
  kept; still 320 mm off when stopped afterwards (reference points recorded during a wrong pose stay wrong).
- **Geometry cache cleared** once or twice per window of 300 presents ("motion impossible to measure"): effect / no
  effect.
- **Field of view "corrected" eight times in a minute**: each time, all the geometry changed scale. The condition
  (enough rotation) is never met with a still camera: no test could see it.

Remedy: stop guessing. A search through CPU-visible buffers (`urti_camscan.inl`, `urti_camscan` file next to the add-on)
found a 512-byte constant buffer, written before its view is created, several times per frame: `+240` projection by
columns (`p00 0 0 0`, `0 p11 0 0`, `jx jy a -1`, `0 0 b 0`, with `jx`, `jy` the DLSS sub-pixel jitter), `+304` camera to
world (right, up, back, position). `urti.ini`: `camera_cb=512` (per-game setting, read at load); `camera=0` goes back to
the old measurement. Hook on `CreateConstantBufferView`: only views of that size are re-read (about 45 per frame). A
captured draw records the number of the current camera; the computation takes the latest of its window.

Pose, motion (synthesised vectors), field of view and depth law now come from that camera, every frame. No more
witnesses, clearing for impossible measurement, or field correction. The sub-pixel jitter is removed from cached
vertices and put back into each frame's pose. `motion_log=1` keeps the witness measurement, separately, as an
independent check.

Measurements, at real pace, `motion_log=1` (deviation of the read camera from the witnesses, median / 95 %):

| Phase | Correction | Rotation | Pairs kept | Frames per second |
|---|---|---|---|---|
| Still | 0.18 / 0.64 cm | 0° | 95 % | 78 to 80 |
| Slow rotation | 0.15 / 0.90 cm | 0° | 92 % | 60 to 77 |
| Fast rotation | 0.32 / 2.69 cm | 0° | 85 % | 55 to 64 |
| Walk | 0.27 / 1.40 cm | 0° | 86 % | 51 to 68 |

300 presents out of 300 with the effect in every 300-present summary of these phases, no cache clearing, field fixed,
depth law identical to the stored one. Bursts: continuous effect.

Left: shadows go from 4.4 ms to 8 to 15 ms when moving (occlusion doubled in motion, dense forest, 7 M-triangle cache);
the buffer found is specific to that game (the search will serve other games). RDR2: no `camera_cb`, path unchanged.

## v77 — URTI: one game frame at a time (the real "effect / no effect")

User feedback on v76: "it jumps all the time; when paused you see shadows and reflections appear and disappear". v76
had fixed the camera, and its summaries ("300 presents out of 300 with the effect") stayed perfect: they counted
presents, not what the computation had really seen. Two tools changed everything:

- **the game's pause as a test bench**: the game's image no longer moves, so everything that varies comes from URTI
  (`urti_pause_ana.py`); and a burst is triggered by a file, without touching keyboard or mouse;
- **a trace per present** (`urti.ini frame_trace=1`, `urti_trace_ana.py`): executed draws, regions, camera, and a row of
  depth and mask read back at each computation.

Still camera, game not paused: the row's mean depth jumped from 0.0262 to 0.0188, and the mean effect from 0.71 to
0.83; paused, nothing (0.0208 to 0.0209).

**Cause.** With DLSS-G, the game executes its lists on a different queue from the presents. ReShade copied the depth at
the present, at that moment: the game had sometimes already started the next frame, so the copy was half drawn. The
computation was not aligned on frames either: a capture window held 0, part of, one or two game frames.

**Remedy (`frame_sync`, with the game's camera only).** The main depth is copied inside the game's list, at every end of
writing (barrier leaving `DEPTH_WRITE`), into a ring of 12 buffers in the common state (they move from one queue to the
other without a barrier). A marker in the list, read back at execution, tells whether that copy saw at least 60 % of the
previous frame's draws; the first that does closes the window, the rest waits for the next computation. One
computation per complete frame, no other. No marker for 16 presents (menus): old rule.

**Other causes fixed, seen on the bench:**

- sub-pixel jitter: measured on the cascades, it existed on only one frame in five (the rest of the time the difference
  was 0 and the absolute value stayed frozen): mask and reflections laid next to the image, history read at the wrong
  place. It now comes from the game's camera;
- RT scene unstable at the edge of the budget: 6 to 7 meshes entered or left per frame with a still camera (the number
  of draws varies with the sub-pixel jitter, an 18-frame cycle). A mesh keeps its place in the budget + 20 %, at most
  two entries per frame, no return within two seconds, and it moves to the cache on the next frame: 0.2 to 1.5 per
  frame;
- huge meshes (grass, foliage) captured in parts: each part overwrote the previous round's in the cache (identity =
  rank among captured draws). Each part now has its own identities;
- places kept without being drawn (3 frames at most): their vertices stayed where the camera saw them; they now follow
  the camera.

**Two crashes along the way (`DEVICE_REMOVED`), fixed:**

- `INVALID_CALL`: the game puts its depth in `COPY_SOURCE` (state 0x800), and my transition went from a state to itself;
- `DEVICE_HUNG` at load, one time in three: the verification calibration (full 512 MB capture) fell while the game was
  loading. Useless with the game's camera: removed in that case. Also, as a precaution: retired buffers destroyed after
  120 presents (three are only a frame and a half with frame generation), wait if a ring slot is reused before the GPU
  has read it (0 waits measured). The exact cause of the hang is not proven: the test went from 1 crash in 3 to 0 in 4.

Measurements at real pace, frame generation on; jumps of more than 8 % of the row's mean effect from one computation to
the next:

| Phase | Before | After |
|---|---|---|
| Still | 75 of 412 computations | 0 of 510 |
| Rotation | 36 of 489 | 0 of 536 |
| Walk | 63 of 322 | 1 of 402 |
| Run | 25 of 320 | 3 of 442 |
| Fast rotation | 22 of 350 | 1 of 451 |

View `debug_view=11` (depth edges in red over the image, real-pace capture, turning camera): effect at most 3 pixels from
the image (2560-wide screen, 29 pixels per game frame at that speed). 4 launches in a row without a crash, two Alt+Tabs.

Left, seen during these tests: **the HUD and the menu are reflected in the water** (no snapshot before the UI there: the
game does not draw into the screen image with DLSS-G); fine grain stays on reflections; 6.5 depth copies per frame, to
reduce. RDR2 keeps v75: the budget and part changes concern it, to recheck before installing.

## v78 — URTI: sparkles in reflections

User feedback on v77: "when paused, reflections build up and become perfect, but while playing they are not clean, and
odd sparkles stay on screen".

In-game bursts, each effect turned off in turn: without reflections, not a single bright point added by URTI. Everything
came from reflections, mostly in motion (0.015 % of pixels brightened by more than 5 % at rest, 0.37 % while walking, on
foliage and object edges).

**Causes and remedies:**

- **the water mask from a different frame than the depth**: the material target is copied separately, and the copy
  could be from the previous frame (and read while the game was writing the next). In motion, a leaf pixel kept the
  sky's material (0, like water) and received a reflection going anywhere. Copies in a ring of 4 buffers, with a marker
  at execution: the computation takes the one from its frame, and waits for it at most two presents;
- **the water's normal from depth**: shifted every frame with the jitter, it sent the reflection elsewhere every frame.
  Hence the grain that only cleaned up by accumulating, with the game paused. Water is flat and horizontal: its normal is
  now the world's up, read from the game's camera (`reflect_up=0` for the old rule). A "water" material pixel is a mirror
  only if it looks roughly up and three of its four neighbours are water at the same distance too;
- **the reflection history on what is not water**: read off-target in motion, it laid a neighbouring water pixel's
  reflection on the grass and let it fade there slowly (it stays on screen). A pixel without reflection no longer
  receives any, a newly uncovered water pixel takes the reflection right away, the clamp's neighbourhood counts only
  pixels that have a reflection, and a very bright point (a particle read from the image) weighs little in the history;
- **the camera rotation used to reproject reflections** came from the cascades, one frame in ten: it now comes from the
  game's camera.

**Bug found along the way, in v77's `frame_sync`:** after a big hitch, the lag behind the game was never caught up (one
computation per frame), stream regions were reused before being read, and the frame's scene stayed empty (`3 vertices`),
without any error. With several complete frames waiting, only the last one is kept now.

Measurements, bursts; pixels brightened by reflections by more than 5 %, isolated bright points added per frame:

| Where | Old rule | New |
|---|---|---|
| Forest, walking (v77 / v78) | 0.37 %, 32 points | 0.02 %, 17 points |
| Water's edge, side step (`reflect_up=0` / 1) | 6.65 %, 2228 points | 0.17 %, 18 points |
| Water's edge, turning camera | 1.07 %, 100 points | 0.49 %, 80 points |
| Water's edge, still | 0.34 %, 42 points | 0.93 %, 66 points |

The two water's-edge rules were not measured at the same place (the character moved in between): the order of magnitude
of the side step is clear, the "still" line proves nothing (different framing, more bright sky reflected). 3 launches in
a row and 2 Alt+Tabs without a crash.

Left: the game's own low-resolution reflection stays visible under the RT reflection where the latter is weak;
`reflect_f0` at 0.15 and 0.3 changes almost nothing to the eye, kept at 0.04; HUD text is still reflected in the water;
the water is a flat mirror, without ripples (G-buffer normal to try). RDR2 keeps v75: to recheck before installing.

## v79 — URTI: camera lost when the game's reflections are turned off

User feedback: "I turned off the game's reflections, and when I run it's a pixel soup, the denoiser does a bad job". The
session's log said something else: at the moment the option changed, `game camera: 300 frames, 0 new, 0.00 cm`, every
read refused, while running. The last camera stayed frozen: zero motion for smoothing (history read at the wrong place,
hence the soup), kept objects misplaced. Nothing reported it. At the next launch, with the option kept, the 512-byte
buffer no longer held the camera at all: it serves the game's own reflection.

**Fixed:**

- camera lost (no value for 30 presents): the old witness measurement takes over, the cache is cleared, and the same
  when it comes back. No more frozen camera;
- search for another source while it is missing: created views and root-bound buffers, two shapes (projection + camera
  matrix, view-projection + view). Adopted only if the sub-pixel jitter changes, if the source is seen on at least 60 %
  of presents for at least 30, with few values per frame, and without teleporting (5 m). Afterwards, dropped if it
  exceeds six values per present or jumps by more than 10 m.

**Not solved: no safe source with the game's reflections off.** Only the draws' buffers remain (world-view-projection +
world-view). Rotated objects look like a camera there (false candidates: 256, 768, 4608 bytes, 70,000 "cameras" for
300 frames, jumps of hundreds of metres: that is what imposed the continuity rule). Trying the most frequent value of
each frame (objects without displacement carry the true camera): median 0.44 cm against the witnesses but 95th
percentile 172 cm; it alternates with the camera seen from the character's frame, then, once that one is discarded,
with large objects' frames (60 % continuity). Removed: a camera that is sometimes wrong is worse than the witness
measurement.

Checked: with the game's reflections off, 2 launches and 2 Alt+Tabs without a crash, witness measurement active, full
RT scene (800,000 to 900,000 vertices).

To do: a safe source without the game's reflections (screen passes that remain: lighting, haze, DLSS); or mask the
blocks of the game's reflection while keeping it on.

## v80 — URTI: reflections taken before the HUD, UI left intact, colours learned per object

User feedback: "we see the HUD bar and the menus in the water; wouldn't it be smarter to use our own reflections that we
learn?". URTI's reflections took their colours from the screen's final image, HUD and menus included: no snapshot before
the UI was possible (`photos 0/0`), the game never draws into the screen image with DLSS-G. And the composition darkened
the HUD and the menu text like the rest of the image.

**The game's image without its UI.** The pass survey (`urti_passes`) shows, at the end of each frame, a 2560×1440 RGBA
16-bit float target that the game writes before laying its HUD: the one DLSS-G receives without UI. Its alpha is
exactly the UI's shape (5.3 % of pixels in the capture: compass, prompts, health; the same as the UI layer written just
after).

- recognised at execution, without any setting: among screen-sized RGBA 16-bit float images at the end of their
  writing, the first one finished after the frame's water mask (after the depth if there is no mask). Adopted when it is
  the same resource at least 8 times out of 10 (in game: 40 out of 40, a single candidate, at every launch);
- copied inside the game's lists at the end of its writing (ring of 4, common state, like the depth), marker read at
  execution;
- converted back into the screen image's layout (`live_hudless.hlsl`), once per new copy (generated frames reuse the
  last): reflections, the surface cache and the light measurement take their colours from it;
- UI mask for the composition (left intact): the alpha (the HUD), plus still pixels (identical in the previous copy)
  clearly different from the final image two conversions in a row (the pause menu, which is not in the alpha).
  Tolerance 0.06 + 15 % of the value. A moving pixel is not compared;
- `hudless=0` in urti.ini goes back to the old path; `hudless=2` runs the conversion without using it (to measure its
  cost).

**Pitfall: the screen image is not encoded like this image.** The target's values match the game's other targets (PQ),
but the screen image is about twice the target (PQ codes; raw readback of a burst: constant ratio, median error 1.3 %,
much better than a PQ law or a square root). The game applies that factor afterwards. Copied as is, the snapshot was
four times too dark in the highlights (reflections darkened the water), and the comparison took 95 % of the image for UI
(no effect at all, still camera). The ratio is learned continuously on the GPU (`live_hudgain.hlsl`): median of the
per-pixel ratio over still pixels outside the UI (log2 histogram, 1.1 % per bin), running average. It varies with the
scene: 2.05 to 2.14 at the water's edge, 1.37 to 1.78 at a campfire and while loading, 1.73 to 1.89 in pause (the game
darkens the background behind the menu). A first attempt with a sum filtered by the ratio already learned: a wrong ratio
taken while loading discarded the good pixels and stayed wrong (1.3 to 1.5); the unfiltered median does not trap itself.

**Colours learned per object of the geometry cache.** A reflection hitting a hidden or off-screen object only had a
colour for the frame's geometry (per-triangle surface cache); on a cached object, nothing: the game's blocky reflection
showed underneath. Each cached object now has a place in a table of 256 colours (one per group of neighbouring
triangles), learned by the surface cache while the object is on screen; the instance identity carries the place and its
generation, the hit-group field the triangle count. The reflection uses them when the hit point is not visible. No
separate measurement of its effect.

**Measured in game** (day, game camera read, real-pace bursts):

- pause-menu text: 97 to 98 % of bright pixels left intact (before: darkened by 15 to 38 levels); in-game HUD with a
  still camera 64 to 88 % intact (before: 24 %, darkened by 25 levels). The reflections' snapshot no longer contains the
  menu or the HUD;
- UI taken for 0.0 to 0.2 % of the screen in game, 1.8 % in pause;
- change of the effect from one frame to the next (pixels > 0.05), still camera: 0.83 % (old path 0.70 %; without the
  two conversions in a row 1.38 %, grass made the mask blink); in pause 0.18 % (old path 0.62 %);
- bright points added: 29 in pause, 86 still, 114 to 120 in motion (old path, another view: 240, 69, 133 to 488);
- cost at the same place, still camera: composition 1.47 → 1.93 ms per present (conversion 0.25 ms, use 0.25 ms);
- about ten test launches, then 4 check launches and 4 Alt+Tabs on the final version, without a crash.

Not done: the light bounce does not use the learned object colours yet.

## v81 — URTI: the reflection no longer stutters with DLSS-G, faster rays, real-pace films and ꟻLIP

User feedback on v80: "the fps must be fixed, we lose too much; at rest the reflections build up and look good, but in
motion they shift, don't follow, the character shifts, it wriggles in the water, it looks like layers above the water;
we need an image analysis that shows whether it's good or not (the NVIDIA thing)".

**Measurement tools.**

- Real-pace film (`urti_film` file): 30 consecutive presents, generated ones included, before and after URTI, at half
  resolution, kept in video memory and written only at the end (a burst writes each frame: the game slows down and only
  computed frames end up in it). One `FILM` line per present (generated, computed); at the end, the depth and the water
  mask of the last computation (`profondeur.bin`, `materiau.bin`, `FILMCAM` line).
- `tools/reshade/urti_film_ana.py`: ꟻLIP (NVIDIA, flip-evaluator 1.7) between each present and the next, on the game's
  image and on the image with URTI; the excess is what URTI adds as perceived change (shaking, jumping). Also the shift
  of the image and of URTI's layer from one present to the next (phase correlation).
- `tools/reshade/urti_eau_hauteur.py`: height of each water pixel from the film's depth.
- Detailed GPU timing ("GPU detail" log): every step of the computation and the composition.

**Cause of the shift in motion: URTI's layer frozen on generated frames.** With DLSS-G, one present in two or three has
no computation: URTI laid the last computation's shadow and reflection on it as they were. Film of a side step: the
image moves 1 to 3 px per present, URTI's layer stays at 0.01 px one time in three, then catches up at once (9 presents
frozen out of 29). Fixed: the reflection resolver keeps, for each pixel, the surface's motion and that of the virtual
point behind the mirror; on a generated frame, the composition reads shadow and reflection at "here + 0.5 × motion"
(`fg_shift=`, 0: old rule). Measured in the same session: 1 frozen present out of 29 (instead of 9), the layer follows
the image on every present (ratio 1.1, constant).

**Not the cause:**

- the mirror's height: on open water, the game's depth is a plane (−1.59 to −1.61 m below the camera): it is indeed the
  surface;
- dark spots around the character's legs in the water: they are in the game's image without URTI (frame generation on
  fast-moving legs). But pieces of the character (gear, hair) could enter the geometry cache, where an object is assumed
  still: objects closer than 6 m to the camera are excluded from the cache (about 300 discarded per 300 frames).

**Cost.** Per computation (water's edge): geometry cache 1.9 ms, frame structure 1.8, occlusion 2.2, denoising 2.0,
reflections 2.3; per present: UI-free image 0.35, stabilised reflections 0.37, composition 0.5.

- Rays: every occlusion and reflection ray examined each triangle it met (ground, rocks, trunks) to know whether it was
  a leaf hole. Now an opaque ray first (the hardware finds the closest); if it is a hole seen on screen, the next opaque
  ray starts just after it (up to six), the slow search only beyond. Same result. Measured at the same place (alternated,
  30 s each): facing the water 80.5 instead of 71 fps (reflections 2.2 instead of 3.9 ms, occlusion 1.95 instead of
  2.85), facing the forest 86.3 instead of 79.5. `rayons_lents=1` goes back to the old rule.
- Denoising: occlusion is computed on only one pixel per 2×2 block each frame; the 5×5 blur read 25 pixels to keep only
  4 to 9. It now reads only those (same result): 2.0 → 1.4 to 1.6 ms.
- Geometry cache: 80 objects and 400,000 vertices rebuilt every frame; now new ones first (300,000 vertices), objects
  already kept refreshed at 100,000 vertices per frame (`geo_new=`, `geo_refresh=`): 1.2 → 0.5 ms.
- Occlusion: no more extra rays where the image moves (`ao_boost=1` to bring them back).

**Left:** the frame structure rebuilt at each computation (2.2 ms), occlusion and reflections in dense foliage (up to
10 ms while walking), composition on every present. Reflections of hidden or off-screen objects take colours learned
per triangle or per group of triangles: flat patches (reflected trunks as pale bars), perhaps the "layers" the user
sees.

## v82 — URTI: a time budget on occlusion and reflections, no more wasted work outside reflective areas

User feedback on v81: "the water must be optimised, we drop to 15 fps, everything is computed all the time, reflections
sometimes take time to build up, a huge number of areas are computed for nothing".

**What the session's log said.** At times, averaged over 300 frames: occlusion 45 ms, reflections 20 ms (running, 50 cm
and 1.5° per frame, dense vegetation, cache of 2,400 objects). Before v81 the maximum was 15 to 20 ms: **v81 had made the
worst case worse**. Hopping from hole to hole (up to six opaque rays) was followed by an unbounded search if the last
point was still a hole: in dense grass, up to seven traversals per ray. It had only been measured with a still camera.

**Bugs found:**

- the occlusion-ray regulator read the wrong interval (markers 2 to 3: the copy of the game's map, 0.02 ms, instead of
  3 to 4) since a marker had been added: it always believed it had room, and occlusion stayed at four rays whatever its
  cost;
- one extra ray per water pixel for reflection anchoring, while anchoring is off (`anchor_alpha=0`);
- reflections computed normal and flatness over the whole screen before reading the material;
- occlusion alone (sun shadows off) computed two normals and read the game's map on every pixel, and the flatness plus an
  8-point neighbourhood (twice) even on the 3 pixels out of 4 without a ray this frame.

**Fixed:**

- bounded rays: a few hops at most (occlusion 0 to 2, reflections 1 to 3), never an unbounded search (`rayons_lents=1` to
  compare). At the end of the hops: occlusion without an occluder, reflection on the last point hit;
- material first: outside reflective surfaces, a reflection costs one read; with the game's camera known, reflections
  on water only (`reflect_all=1` for other smooth surfaces);
- occlusion: pixels without a ray exit right away;
- a time budget per pass, held every frame, targets `ao_ms=4` (occlusion and denoising) and `refl_ms=2.5` (0: no
  budget). The level (0 to 4) rises as soon as the pass exceeds its target and comes down slowly. Occlusion: rays 4 to 1,
  hops 2 to 0, range 50 to 10 m (faded smoothly). Reflections: hops 3 to 1, range 500 to 100 m, one pixel in 1, 2 or 4
  gets a ray (the pattern rotates), the others take their neighbours' colour (`live_reflresolve`).

**Measured in game,** same route (running back and forth through vegetation and along the water, two alternated laps,
"time budget" log):

| | no budget | with budget |
|---|---|---|
| heaviest stretch | 39 to 46 fps (occlusion 12 to 17 ms, reflections up to 12 ms) | 74 to 82 (3.6 ms, 2.7 ms) |
| worst frame, occlusion | 32 ms | 5 to 7 ms, one 16 ms peak when entering the area |
| worst frame, reflections | 27 ms | 6 ms |

Every reflective area is water (`reflect_stats=1`: 8 to 25 % of the screen, 80,000 to 600,000 rays per frame). Film at
the water's edge with the budget: clean image (no checkerboard, no blocks), ꟻLIP of the same order as before.

Reflections that take time: at rest, the reflection took only 10 % of the new value per frame; raised to 25 %
(`refl_alpha_min=`). Measured with ꟻLIP (alternated, twice): at rest, no more shaking (excess 0.0060 and 0.0044 instead of
0.0058 and 0.0053); after a camera move, less spurious change (0.0156 and 0.0148 instead of 0.0214 and 0.0294).

## v83 — URTI: the game's blocky reflection under URTI's, Red Dead updated

User feedback on v82: some reflection bugs in the second game; "on Red Dead it's clean; on Red Dead, in town when you walk,
shadow lines appear".

**Measured first: URTI adds no colour cast.** Films, each effect turned off in turn: green share of the image 0.343
without URTI, 0.341 with it; neither the bounce, nor occlusion, nor reflections add any. The green is the game's water
colour.

**What made it visible everywhere: v82's budget.** Reflection budget forced to its lowest (`refl_ms=0.4`): half of the
water turns bright green. The range of reflected rays dropped from 500 to 100 m: the dark reflection of distant trees
disappeared and let the game's colour through. Fixed: the range never drops any more; the budget acts on hops, and on
one pixel in two at most (no longer one in four); a pixel without a ray this frame mostly keeps its history (clamped by
its neighbours), the neighbours' average only corrects it. Occlusion: the range drops to 25 m at most (10 m before),
and in the distance one or two rays are enough (beyond 25 m, a single one). Reflection target 3.5 ms.

**The "layers": the game's reflection under URTI's.** The game draws its own reflection into the water, in large
low-resolution blocks; through URTI's sharp reflection (Fresnel weight only), those blocks stayed visible: two
reflections that do not coincide. Fixed (`live_waterbase.hlsl`, game camera known): the game's water colour smoothed
(mean of water pixels per block of 8 render pixels, 7×7 blur weighted by the water share), replacing the game's colour
where the reflection matters. The game's reflection is much stronger than Fresnel alone: smoothed and nothing more, the
water became dull. URTI's reflection now weighs 3 times Fresnel on water (`water_gain=`, 2 to 4 tried). `water_base=0`
goes back to the old composition.

**Red Dead.** Shadow lines: not found. Bursts while walking in a Saint Denis courtyard (with and without kept places,
`persist=0`), installed version then new version: 0 fixed lines, at most one fleeting line per frame. The long thin
shadows of wires (tram, telegraph) are in the game's image without URTI (v50 split-screen capture). New version
installed in Red Dead (the old one kept as `urti.addon64.avant_v83`): 76 to 78 fps instead of 71 to 76 at the same
place, occlusion 1.8 → 1.4 ms, composition 0.9 → 0.6 ms, no crash. The reflection keeps the old smoothing there (10 % per
frame): without a game camera, its normal comes from depth.

Not checked: Red Dead's wet-ground reflections with this version (dry weather), the sea, night.

## v84 — URTI: world radiance cache, layer realigned on the displayed image, HUD protected in motion

User feedback on v83: "the reflections are broken; we need real ray tracing, we have the information, we just have to
inject it all properly; use NVIDIA's techniques".

**What the films showed (game on the left, URTI on the right).** URTI's water was dull and grey, the sky was hardly
reflected any more, and while walking the reflection broke into flat-coloured shards. Cause: a hidden or off-screen
reflected point received a single colour per 256th of an object (a 200,000-triangle tree: 800 triangles per colour), and
a ray going off screen toward the sky received nothing (the game's water showed again).

**World radiance cache** (`live_radcache.hlsl`, `live_radhash.hlsli`, after NVIDIA's SHaRC; game camera known). A
spatial hash table of 2^23 cells (128 MB) in world space: on each computed frame, one pixel in four deposits the colour
the image shows into the cell it is in, at three levels (a cell of 0.5 % of the distance, i.e. a few pixels, and two
coarser ones). A reflection hitting a hidden or off-screen point reads back the colour seen there, blended over the
eight neighbouring cells. Each cell also keeps its solid share: one pixel in sixteen recasts the camera ray into the RT
scene, and each triangle crossed before the visible surface is a hole (cut-out foliage card); off screen, the reflected
ray crosses foliage according to that learned share. Sky and distance map (512 × 512 octahedral, per world direction): a
ray that hits nothing takes its colour there; directions never seen fill in from their neighbours. Extra per-pixel output
(`reflMix`): colour already known and the share to read from the image. `radcache=0` goes back to per-object colours.
Counters with `reflect_stats=1`. First try: a 2^22 table full in under a minute (397,000 deposits refused per frame);
with 2^23 and a cell reclaimed after 2 s unused: 2,000 to 14,000.

**URTI's layer was two game frames ahead of the screen.** Found while redoing the HUD mask: with DLSS-G, the final image
of a present is not that of the last copy of the UI-free image, but of an older copy (comparison on one pixel in 64: most
often the oldest of the last three). View 11 while turning: depth edges of the computation 27 to 37 pixels (half
resolution) away from the image's, for 8 to 9 pixels of motion per present. Fixed: `live_hudgain` tracks which game
frame each present shows and writes the layer's lead (1.4 to 1.7 game frames on average); `live_composite` slides
shadow and reflection by that much along the motion (replaces v81's half step on generated frames). Same rotation
afterwards: 0 to 1 pixel on most presents. `lag=0`: old rule; `lag_bias=`: correction.

**HUD.** The UI-free image's alpha is 0 everywhere in the session (it only covers some screens): the mask held only on a
still background (v80), and the reflection went over the HUD above the water. Redone: mask = difference between the final
image and the copy that matches it, UI share from 0 to 1, redone only on presents where a copy matches at 96 % (150 to
290 out of 300). The law between the two images is not a ratio: final = k × (UI-free)^1.13 to 1.25 per channel
(measured), learned continuously (medians of dark and bright pixels).

**Composition.** With the real sky in the reflection, v83's weight of 3 × Fresnel made a milky mirror hiding the bottom
of the water: 1.5 by default (1 to 3 compared at the same place), and the game's water is smoothed only where URTI's
reflection dominates.

**Measurements (RTX 4070, DLSS + DLSS-G).** 88 to 92 fps at rest by the water, 65 to 80 in motion (v83: 92, 70 to 84);
world cache about 0.4 ms per present. Two launches, running, two Alt+Tabs: no crash. Left: bright spots or blocks in
places in the reflection (coarse cells, foliage), a band at the screen's edge without realignment in motion, perfectly
flat water (no ripples).

## v85 — URTI: layer queue (no more hitches), the game's water left alive

User feedback on v84: "the reflections are gorgeous but it stutters in every direction, the character lags and even the
ground; near the water the foam pops in and out, the water freezes at times, and we lose a lot of fps".

**Hitches.** v84 slid the last layer by the measured lead (1.5 to 2 game frames) along the motion: right on average,
wrong as soon as the game's pace varies (film while turning: still image, layer at −3.8 then −7.4 then 0 pixels from one
present to the next). Matching exactly the computation and its UI-free copy (a marker left in the lists' execution
order) changed nothing: the computation's copy is the last one 96 % of the time. Fixed with a queue (`live_delay.hlsl`):
the layer of each computed frame (shadow, stabilised reflection, motion, jitter) is stored in one of three slices with
the number of its game frame; `live_composite` lays on each present the slice of the game frame it shows, and only
catches up the remainder along the motion (half a frame, frames made by DLSS-G). Same film afterwards: image −4.70 /
−12.73 / −5.19 pixels, layer −4.10 / −10.15 / −4.71. Change of the layer from one present to the next while walking: 16
and 12 levels → 7.4 and 5.9; URTI's ꟻLIP excess: 24 % of what the game changes itself → 15 % (walk), 22 % → 11 %
(rotation).

**Water.** The game's water smoothed under the reflection (v83) was redone only on computed frames and erased the foam
where the reflection passed a threshold: water frozen at times, foam popping in and out. `water_base=0` by default: the
game's water stays as it is under the reflection. Anchoring ray (one extra ray per water pixel, no effect on cached
objects) removed along with it.

Left: cost of reflections near a large stretch of water (3 to 5 ms, budget at its lowest in the player's session),
reflections off water (`reflect_all=1`) to validate, coarse colour patches in places in the reflection.

## v86 — URTI: half the reflection rays without flicker, and where the time really goes

User feedback on v85: "much better but there's still quite a lot of noise; we need good performance". Session log:
occlusion and reflections at level 4 all the time, still 6 ms each, 48 to 55 fps.

**Noise.** Beyond level 2, one water pixel in two had no ray and took half of its neighbours' average: one frame a real
ray, the next an average, so thin reflected objects flickered. A pixel without a ray now keeps only its history (it got a
ray the frame before); hence one pixel in two at any level (`refl_full=1`: old rule). Occlusion: never a single ray any
more, the range drops instead (50 to 12 m). Reflection range reduced when the budget is exceeded (500 to 90 m): with the
world cache, a shorter ray ends on the sky and distance map, no longer on the game's water. World cache filled on one
computed frame in two, HUD mask redone on one present in four.

**Measurements by the water.** Reflections 2.8 to 3.7 ms → 2.0 to 2.6; composition 1.9 to 2.75 → 1.4 to 1.6. ꟻLIP excess
at rest 0.0099 → 0.0067; walking and running unchanged (0.019 / 0.021; 0.023 / 0.018).

**Where the time goes (each effect turned off in turn, at rest).** Game alone 131 fps; URTI with occlusion, bounce and
reflections off: 98; reflections alone off: 93; everything on: 84 to 90. The effects cost only 5 fps each: the bulk is
fixed, per game frame: frame scene 2.1 ms, denoiser 1.2 ms (even with occlusion off), geometry cache 0.55, scene
structure 0.3. Lowering the budgets changes almost nothing (occlusion 3.4 ms at level 4 against 3.9 at level 0).
`blas_refit=1`, `geo_new=100000`, `vertex_budget=200000`: frame structure 2.05 → 1.24 ms, 1 to 2 fps. Next lead: stop
redoing everything every game frame (the layer queue makes it possible to serve two frames with one computation).

## v87 — URTI: objects vanishing from reflections, Red Dead checked

**Red Dead Redemption 2 with v86** (never tried there): two launches, walking, turning, bursts: correct image, minimap
intact, 70 to 77 fps (v83: 76 to 78), no crash. Installed (old one: `urti.addon64.avant_v86`). The real-pace film
(`urti_film`) loses the device there (DXGI_ERROR_INVALID_CALL): do not trigger it in Red Dead.

**Objects appearing and disappearing.** Player's log: 975 objects evicted from the geometry cache's reserve in 300
presents, reserve half empty. The reserve is a ring buffer: in motion (300,000 new vertices per frame) its head goes
round it in a few seconds and evicted whatever it met, visible objects included, which were then missing from
reflections and occlusion until their next capture. The head now skips objects captured within the last 600 frames.
Same run: 315 to 333 evicted (the old ones). Vertices measured per frame: 500,000 → 250,000.

`reflect_all=1` in a forest: no visible difference (no smooth surface off water there), 0.4 ms: left off.

## v88 — URTI: whole-layer flashes after a hitch, reflections of thin objects, denoiser tests

User feedback: "noise on thin objects in both games, the denoiser must be handled well".

**Where the noise is (measured).** Red Dead, burst at rest: URTI's frame-to-frame excess is on 0.3 % of pixels. Second
game, film at rest: almost all of it in the water reflection, on thin reflected objects and the bank line. Averages per
film varied threefold for the same setting: an intermittent event dominated.

**Whole-layer flashes.** Three films out of ten at rest held a jump: for one present, the whole layer changes (reflection
missing or of another tint, occlusion of the character), then comes back. Trace: each time after a game hitch (a present
at 37 ms instead of 12), URTI chains three catch-up computations whose layers differ from the others, shown two frames
later by the queue. A layer computed in the three computations following a present more than 2.2 times longer than usual
is no longer stored in the queue: the previous one stays, realigned on the motion (up to four frames apart). Sixteen
films at rest afterwards: no jump above 0.05 (before: 0.10 to 0.16), mean 0.005 to 0.008 (before 0.007 to 0.018). A first
rule tried (computation on the present following a computation) discarded up to 80 layers out of 300 presents in motion,
and the layer no longer followed the image while turning; abandoned.

**Reflections.** Reflected colour read over a small area (four reads, rotated grid, 1 pixel: `refl_soft=`) instead of a
point: with the per-frame sub-pixel jitter, a perfect mirror fell sometimes on a branch, sometimes next to it. Worst jump
at rest 0.06 to 0.13 → 0.016 to 0.034. Spatial pre-filter before accumulation (`refl_spatial=0.5`: the pixel blended with
the average of its reflecting neighbours): small gain.

**Occlusion denoiser.** History length per pixel (`denoise_len=`): a newly uncovered pixel takes the true average of its
first samples. Gain not measurable on the films; kept (free). Blur widened while the history is short: +2 ms, removed.
Thin objects filled with their neighbourhood's occlusion: eight times more noise in Red Dead's foliage (2.8 % of pixels
instead of 0.3 %), removed.

**Checked.** Red Dead: walking, turning, bursts, no crash, 67 to 71 fps, noise unchanged (0.34 % of pixels).

## v89 — URTI: heavy scenes, single-cell world-cache lookup

Player's log (v88): at a busy place, occlusion and reflections permanently at the maximum saving level, and still up to
14.5 and 17.4 ms (142 frames out of 300 above 8 ms for reflections), 41 to 71 fps. Two causes: the world-cache lookup read
the eight neighbouring cells (up to forty memory reads per point, at every foliage crossing); the last level did not go
down far enough.

Lookup in a single cell, at a position drawn at random within a neighbourhood the size of a cell, new every frame (as in
NVIDIA's SHaRC): smoothing comes from the reflections' accumulation. Last level: reflections on one pixel in four and
without foliage hops, occlusion without hops. Same place by the water, against v88: stabilised reflections 1.2 → 0.9 ms;
maximum level forced: reflections 0.55-0.9 → 0.26-0.69 ms. Flicker unchanged (at rest 0.0055-0.0071 against
0.0061-0.0069; walking 0.0155 against 0.0160).

Checked: Red Dead walking, turning, bursts, 62 to 70 fps, noise unchanged (0.35 % of pixels).

## v90 — URTI: flickering lines in Red Dead's streets, nearly empty frame layers

**Red Dead, "lines" in town.** Bursts at rest on a Saint Denis balcony, each effect turned off in turn: flickering pixels
(more than 3 levels from one frame to the next, on top of the game) 0.46 % all on, 0.11 % without reflections, 0.38 %
without the bounce, 0.38 % without occlusion. Reflections make most of it, and the map shows diagonal lines in the
street: tram rails, cobblestone edges, kerbs. An isolated smooth pixel received a reflection whose direction, derived
from depth (no game camera in Red Dead), changed every frame. Reflections now only on an extended smooth surface: at
least 5 of the 8 neighbours 2 pixels away smooth too and on the same surface (`refl_coherent=0`: old rule). Same place,
twice each: 0.19-0.20 % → 0.06 %.

**Remaining flashes.** A film at rest still showed two strong jumps (0.10-0.12): a computation whose frame had only 13
game draws executed before the present (usually 3,800; the game's lists had gone into the previous window), without a
hitch long enough for v88's rule. A computation with less than a tenth of the usual draws is no longer stored in the
layer queue. Sixteen films at rest: no jump above 0.06 any more; three small ones (0.05-0.06, layer 7 levels instead of
15 to 28).

Checked: Red Dead walking, turning, bursts, 67 to 72 fps.

## v91 — URTI: shaking reflections, budget regulator that followed peaks

User feedback: "a lot of noise on textures, flicker on thin places, reflections shake".

**Who makes the noise.** Same back-and-forth by the water, each effect turned off (mean ꟻLIP excess, at rest / walking):
all 0.0091 / 0.0180; without occlusion or bounce 0.0104 / 0.0230; without bounce 0.0093 / 0.0191; without reflections
0.0057 / 0.0120; occlusion without grass 0.0082 / 0.0180. The reflections.

**Reflections.** At rest, twice each: baseline 0.0069 and 0.0059; 10 % new per frame (instead of 25) 0.0045 and 0.0053;
one ray per pixel 0.0044; both 0.0040. Walking, no difference (0.010-0.011). Both kept (`refl_alpha_min` 0.1,
`refl_full=1`: one pixel in two only when the budget asks for it). Reflection cost by the water 2.4 → up to 3.5 ms (the
budget handles it).

**Regulator.** Player's log: "occlusion" peaks of 25 to 50 ms (the game takes the queue at the same moment); one peak
raised the level by two, and quality came back only one level every 40 frames: occlusion at level 2 to 3.7 on average,
so fewer rays and less range, more noise. The decision is now made on the median of the last 9 frames (up one level if it
exceeds the target by 25 %, back down if it is under 65 %, every 30 frames).

Checked: Red Dead bursts, 67 to 70 fps, noise 0.10 % of pixels.

## v92 — URTI: where the fixed cost goes (measurements), counters in the log

New log lines every 300 presents: present durations (median, P95, P99, worst), CPU time spent intercepting draws, draws
and vertices replayed by kind (main screen, sun map, transparents, other screens, measurement area).

**Second game, by the water, at rest** (median present duration): URTI off 7.5 ms (131 fps); URTI without effects, almost
nothing replayed 9.2-9.3 ms (108); without effects 10.1-10.5 ms (98-101); everything on 13.0-14.3 ms (74-80).

- Draw interception: 3.2 to 4.5 ms of CPU per present (~2 µs per draw, ~2,000 draws). Removing the global lock of the
  binding events (each list's state read without a lock) changed nothing: neither the capture time nor the frame rate.
  Removed.
- Replayed vertices: 1.4 million per present, 1 million of them for the sun map, useless there (sun shadows off, and the
  sun direction measured on 0 % of frames). Replayed by eighths (`sun_eighth=1`): 0.6 million, a gain within measurement
  noise (± 3 fps). Left off by default.
- The fixed cost is explained by the timed GPU work: per game frame, frame scene ~2 ms, shadow pass and denoiser ~1.2 ms
  (even with occlusion off), composition ~0.45 ms per present, structure 0.6 ms: ~5.2 ms, the measured gap (98 against
  131 fps).
- Frame scene: 822,000 vertices → 2.0 ms; budget 250,000 → 533,000 vertices, 1.56 ms, ~80 fps; budget 120,000 → 241,000
  vertices, 1.30 ms but 66 fps (occlusion and composition more expensive); update instead of rebuild: the scene changes
  almost every frame, no gain. About 1.1 ms of the rebuild does not depend on size.

Red Dead: 70 to 71 fps, 3,800 draws seen per present, 5.1 ms of CPU to intercept them, 1.55 million vertices replayed
(main screen).

## v93 — URTI: denoiser skipped out of range; two leads closed

- Occlusion and bounce denoiser: pixels beyond the occlusion range (where the ray pass writes "nothing") are no longer
  filtered (`denoise_far=0`: old rule). 1.18-1.26 → 0.94-1.05 ms per game frame, image unchanged.
- Tried and removed: unused vertices of the frame scene marked inactive (NaN) instead of flat triangles. Rebuild 2.1 →
  1.9 ms, but rays slightly slower (occlusion 1.54 → 1.64-1.80 ms, reflections 2.36 → 2.67-2.95): nothing gained.

URTI's cost summary (everything on, per game frame, ~10.5 ms, two presents per frame with DLSS-G): frame scene ~2 ms,
occlusion ~1.5 ms, denoiser ~1 ms, reflections 2.3-2.7 ms, composition and stabilised reflections ~2.1 ms, structure
0.65 ms. No single big waste: the next gains come from less work per frame (effects computed in turn) rather than 0.2 ms
savings.

Checked: Red Dead 71 fps, noise 0.17 % of pixels.

## v94 — URTI: G-buffer normals for reflections; Red Dead's camera orientation

- The game's exact normals read from the G-buffer (`gnormal_slot=`, `gnormal=`, `gnormal_signs=`): either octahedral RG16
  in camera space (`gnormal=1`), or RGB8 in world space as in Red Dead (`gnormal_slot=1`, `gnormal=2`).
- Red Dead: the game's camera is not read, so its orientation is solved every frame on the GPU (`live_rotfit`, Horn's
  method) from ~13,000 flat pixels: it follows the mouse, and the camera's up falls back on the world's Z. Agreement
  0.46-0.82 depending on the scene (texture relief is in those normals); the wrong rotation would give ~0: threshold
  0.35. A sampling ran only one frame in four (quarter-rate occlusion): fixed.
- Reflections: the game's normal replaces the depth normal when it faces the camera and roughly agrees with it
  (`gnormal_reflect=1`, active as soon as `gnormal=` is set).
- Measured (bursts, share of pixels changing by more than 3 levels): Red Dead 0.09 / 0.04 / 0.04 % with it against
  0.04 / 0.05 % without, nothing gained (the cobblestones' relief moves with the jitter). Left off by default.
- URTI's noise in Red Dead, effect by effect (bursts at rest and while turning the camera): 0.00 to 0.06 % of pixels,
  while the game's own image varies by 2 to 5.5 levels per frame (wind in the foliage, the game's temporal smoothing).

Checked: Red Dead five launches (one crash caused by opening the Social Club).

## v95 — URTI: sun found again in Red Dead, ReShade menu checkboxes

- **Sun lost in Red Dead since v74** (0 % of frames): a calibration had taken for the sun map a 1024 × 1024 depth seen
  along the camera's axis (sun (0.002 0.059 0.998)) and stored it in `urti_calib.ini`; since RT sun shadows are off, no
  calibration replaced it any more. Fixed: a direction within 14° of the camera axis is refused; in a game without a read
  camera, the sun is searched for as long as it is not tracked, **only in game** (an RT scene of at least 20 draws),
  more and more spaced out without a sun (up to 32 intervals). A test that searched from the menu, every 120 frames, led
  Red Dead to "out of memory": discarded. Measured: sun tracked on 100 % of frames, 46 pairs per frame, 4 cascades, 0.04°
  from one frame to the next; sun / sky measured 0.21-0.25 (overcast evening). 60 fps.
- **ReShade menu**: checkboxes "Ray-traced reflections", "Ray-traced ambient occlusion (RTAO)" (strength, range in
  metres, 2.5 by default: 1.5 before in Red Dead), "Ray-traced indirect lighting (RTGI)" (strength); read on every
  present, they multiply the urti.ini settings. The bounce keeps its rays even if occlusion is unticked. No checkbox yet
  for sun shadows (the current mode turns the bounce off).
- Finding (burst, same image before / after): URTI darkens 3.5 % of pixels by more than 5 % (ivy, wall corners, foot of
  the wall), 0.3 % by more than 15 %; the bounce brightens nothing visible. Starting point of the audit for real RT
  lighting.

## v96 — URTI: RT occlusion on every surface and long-range sky visibility

- **The game's exact normals for occlusion** (`gnormal_use=1`): in Red Dead, those of the G-buffer (world space) rotated
  by the camera orientation solved in the previous frame (live_rotfit). Occlusion now also applies to surfaces that are
  not flat (trunks, rocks, objects, characters), where the depth normal was only noise. Foliage seen through stays
  excluded. Comparison and rotation are measured only on flat surfaces (otherwise depth noise would skew them).
- **Long-range sky visibility** (`ao_sky=0.6`, `ao_sky_range=12` m; "Long-range ambient shadow" slider in ReShade): one or
  two long rays per computed pixel tell whether the canopy, a roof or a cliff hides the sky; they remove only sky light.
  **Only directions above the horizon count** (reflected by symmetry, weighted by their height): a first try counted the
  ground below vertical faces as hidden sky and darkened walls and characters in one block (26.7 % of pixels by more than
  5 %, a uniformly grey wall). The world's up comes from the camera orientation: without it, no long rays.
- Measurements in Red Dead (bursts, same image before / after, share of pixels darkened by more than 5 % / 15 %): v95
  4.2 / 0.4 %; contact with the game's normals 6.6 / 0.5 %; with the sky 13.2 / 3.1 % (foot of the wall, under the cart,
  trees, sheltered ground). Added flicker 0.11 % of pixels at rest, 0.00 % while turning the camera. Occlusion pass 1.2 →
  2.5 ms.
- Red Dead settings added: `gnormal_slot=1`, `gnormal=2`, `gnormal_use=1`, `gnormal_reflect=0` (previous urti.ini kept as
  `urti.ini.avant_v96`).

## v97 — URTI: "bold" occlusion by default, thin objects discarded

- Player's choice: a bold effect. Physical values instead of cautious ones: `ao=1` (0.8 before), `ao_sky=1` (0.6),
  `ao_sky_range=16` m (12).
- First try: 15-19 % of pixels darkened by more than 5 %, but added flicker 0.37-0.78 % of pixels at rest (0.07 % without
  the long rays), on branches, ivy, fences: thin objects moving in the wind, where one or two long rays pass between the
  leaves or not.
- **Continuous surfaces only** for occlusion with the game's normals and for long rays: 3 pixels away in all four
  directions the depth stays close (ground, walls, trunks, rocks, characters); branches, twigs, foliage edges discarded.
- Measurements (Red Dead, bursts): 14-19 % of pixels darkened by more than 5 %, 4.5-6.2 % by more than 15 % (v95 at the
  same place: 8.4 / 1.8 %); added flicker 0.01-0.16 % at rest, 0.01 % while turning. Occlusion pass 2.7 ms.

## v98 — URTI: occlusion and bounce unticked by default, sun map no longer replayed for nothing

- Player's verdict on v97: occlusion leaves **trails everywhere** (its accumulation over time does not follow motion),
  the result is "really not great"; only reflections are good. The bursts measured flicker, not those trails in motion:
  the number did not see them.
- "Occlusion" and "Bounce" checkboxes unticked by default in urti.fx and in the presets (copies
  `ReShadePreset.ini.avant_v98`).
- Without occlusion, bounce or sun shadows, **the sun map is no longer replayed** and the sun no longer searched for (it
  comes back as soon as a box is ticked again): Red Dead 3.8 → 1.7 million vertices replayed per present, capture 5.9 →
  4.8 ms of CPU; shadow pass 0.3 ms.

## v99 — URTI: reflections on glossy surfaces (parquet, waxed wood, metal, cobblestones)

- Player's request: reflections are what works; extend them. Until now only surfaces with smoothness ≥ `smooth_min` (0.5
  in Red Dead) had an RT reflection; Red Dead's floors (~0.36) were excluded.
- `gloss_min=` (0: off; 0.2 in Red Dead): from that smoothness up to `smooth_min`, the reflection fades in (weight
  Fresnel × ramp × `gloss=`, 1 by default); above `smooth_min`, nothing changes. "Reflections on glossy surfaces"
  checkbox in ReShade.
- **Blur by roughness**: lobe half-angle ~ alpha = roughness², footprint at the reflected point = distance × alpha / (eye
  distance + distance), at most 60 render pixels; stored with the reflected point's distance (w = distance + 20,000 ×
  radius: the reflection pass has no room left in its root signature); read by live_reflresolve over an eight-tap disk
  (Vogel spiral rotated per pixel). A named approximation: one mirror ray, then a blurred read, no rays drawn in the lobe
  (no noise; the blur ignores what is hidden on screen).
- Measurements (Red Dead, Saint Denis, bursts): 20.7 % of the screen with a reflection; image changed by more than 5 % on
  6.5 % of pixels (0.2 % without; 3.6 % at threshold 0.3); added flicker 0.12 % at rest, 0.07 % while turning;
  reflections 0.69 ms, composition +0.5 ms; 63 fps.
- Player's verdict: "way too much", reflections in too many places and too far. Put back as before: `gloss_min` removed
  from Red Dead's settings, "Reflections on glossy surfaces" unticked by default (the code stays, off). Next request:
  decide better which surfaces deserve a reflection.

## v100 — URTI: glossy reflections sorted (close floors only)

- Glossy surfaces (smoothness between `gloss_min` and `smooth_min`) sorted with the game's data instead of a threshold
  alone: **a floor** (normal within 32° of the world's up, given by the camera orientation in Red Dead), flat and extended
  (mirror rules), **not foliage or skin** (Red Dead's material type, red channel of its material target: 0.2 and 0.31),
  **close** (faded from 10 to 18 m). Red Dead: `gloss_min=0.3`, `gloss=0.6`. A neural network was ruled out: no training
  data, several ms per frame, and it would guess what the G-buffer gives exactly.
- Measurements (a Saint Denis balcony, street below): 1.2 % of the screen with a reflection (20.7 % in v99); image changed
  by more than 5 % on 0.4 % of pixels; added flicker 0.01 %; reflections 0.22 ms. To judge indoors or under the arcades
  (polished stone 0.36).

## v101 — URTI: puddles sharp again, glossy floors really active, mirror rule restored

- **Regression in v99-v100** (reported by the player: "you can hardly tell what is reflected, puddles used to have bigger
  reflections"): the roughness blur applied to every surface below a smoothness of ~0.86, puddles included (0.60). It now
  applies only to the glossy band (below `smooth_min`): mirrors, water and puddles are sharp again.
- **Glossy floors never active in v100**: the sorting took the world's up from the camera orientation, solved only when
  occlusion runs (off since v98). The floor is now recognised from the world normal of Red Dead's G-buffer (z > 0.85),
  bound to the reflections as soon as it exists.
- **Mirror rule (v90) weakened** when glossy reflections were on: a mirror counted neighbours that were only "glossy";
  ceiling beams of the arcades received a reflection. A mirror again requires mirror neighbours.
- Tried and removed: skipping the shadow pass without occlusion (0.3 ms): the mask stayed at zero, the whole image
  darkened (95 % of pixels changed).
- Measurements under the Saint Denis arcades: polished stone floor reflecting the lit corridor, ceiling unchanged; added
  flicker 0.06 % at rest, 0.00 % while turning; reflections 0.25 ms, 71 fps. Flicker of distant windows and cables: in
  the game's own image (11.5 % of pixels vary by more than 3 levels from one frame to the next on the balcony), not added
  by URTI (0.05 %).

## v102 — URTI: a single menu checkbox, clean occlusion (without characters or foliage, responsive denoiser)

- Player's request: no more visible settings, everything on by default with the best settings; occlusion can be
  beautiful if it neither trails nor flickers. ReShade menu: only the "URTI" checkbox visible; reflections, close glossy
  floors, occlusion and ambient shadow on; light bounce off (invisible, ~0.9 ms). Values stored in the presets cleared
  (copies `ReShadePreset.ini.avant_v102`).
- **Trail measurement** (`trainee.py`): bursts while turning the camera; shift of URTI's darkening map compared with the
  game image's (lag 0: attached) and the deviation left once the previous map is realigned on the image's motion.
- Causes found: occlusion on characters, horses (animated) and the canopy (in the wind); long rays on branches; history
  clamp too wide in motion.
- Fixed: **no occlusion on characters** (alpha of Red Dead's G-buffer normals) **nor on foliage and skin** (material type;
  the material target is now bound to the occlusion pass, t10); long rays reserved for large continuous surfaces
  (continuity at 6 pixels); `ao_sky` 1 → 0.7; history clamp tightened in motion (half at 4 pixels per frame and more).
- Measurements (Red Dead, a tree-lined Saint Denis street): deviation after realignment while turning 4.0-4.7 % →
  1.5-1.7 %; added flicker at rest 1.49 % → 0.22 % of pixels; mean darkening 4-6 % of the image; lag 0.00-0.14.

## v103 — URTI: Red Dead's terrain in the RT scene (tessellated draws)

- Step 1 of the "light transport" the player asked for (sun bounce in nature): the terrain, drawn with tessellation, was
  missing from the RT scene.
- Capture: pipelines with hull + domain shaders (without geometry or mesh shaders, writing depth) keep their code; their
  capture pipeline replays VS + HS + DS (PATCH topology) and stream output takes the position after the domain shader.
  Output size unknown in advance: 32 vertices per control point first, then the mesh's learned size (like
  ExecuteIndirects), 196,608 vertices at most. Setting `tess=1`.
- Measurements (Saint Denis street): tessellated draws ignored 11 → 6 per present (the others do not write the screen
  depth); frame structure 1.19 → 1.71 M vertices, rebuild 1.72 → 2.14 ms; no crash; added flicker 0.25 → 0.35 %; trails:
  deviation while turning 1.44 %. Small visible effect in town; to judge in nature.

## v104 — URTI: occlusion follows motion (moving pixels computed every frame)

- Player feedback: when moving, occlusion "fizzes" on objects, follows motion badly and leaves shadow lines on them.
- Cause: quarter-rate occlusion (one pixel per 2 × 2 block each frame, in turn); in motion, a pixel had a fresh value
  only one frame in four, the rest was old data pasted back with the motion vectors. The pass did not even read the
  game's vectors (only with `ao_boost=1`).
- Fixed: the pass reads each pixel's motion; **a pixel moving by more than one pixel per frame is computed every frame**
  (two contact rays, one long ray), the others stay at quarter rate. Denoiser: in motion, half new (0.3 before).
  `ao_motion=0`: old computation.
- Measurements (Red Dead, Saint Denis, same launch, setting changed on the fly): walking, added flicker 0.10-0.21 % of
  pixels against 0.88-1.85 % (one burst at 1.79 %, the first and most agitated walk); turning, share of the occlusion that
  does not follow 0.36-0.52 × the mean darkening against 0.75. Occlusion pass ~1.2 ms in motion (0.8 at rest).

## v105 — URTI: Red Dead's projection finally right (38°, not 50°), stable and anchored occlusion, dual history, low-discrepancy rays

Request: viable RT occlusion in Red Dead, without offset, measured with precise tools; check whether Red Dead was better
before.

**Tools**
- `decalage.py` (bursts before / after): motion of the image and of the darkening map by phase correlation at a tenth of a
  pixel; **follow** (1: attached, 0: frozen), **fixed offset** of the map relative to the image's edges, **instability**
  (deviation of the map from the previous one realigned on the motion, relative to the mean darkening), darkened share.
  `rdr2_regress.sh`: the same measurement session for any version of the add-on.

**Regressions found**
1. **Field of view**: v70 measured it (37.9°) and corrected the projection; since then, URTI worked at **50°** in Red
   Dead. The measurement (s², t² of the cascades) still gave 37.7-37.8°, but the correction required an accumulated
   rotation (4e-4) never reached, and every calibration (sun search, v95) put 50° back. A camera space stretched by 31 %:
   floor normal, reflection direction and occlusion shape skewed. Fixed: threshold 1e-4, horizontal and vertical stretches
   agreeing within 15 %, **the same measurement within 5 % 30 frames later**, **camera still for 220 frames** (while
   walking the measurement drifted: 37.9° → 60.8° → 91.2°), no more than ~25 % per correction, 25° to 75°; calibration no
   longer puts 50° back; measured field stored (`fov_v=` in urti_calib.ini) and read back at launch.
2. **Grass**: "foliage" (Red Dead's material type) also covers grass: excluded in v102, parks and nature had no occlusion
   (0.5-1.3 % of the image against 18-27 % in v70 / v83). Only grass facing up (world normal, z > 0.6) keeps it; tree
   leaves stay excluded.
3. **Denoiser settings shifted in v104** (a comment in the middle of a line: clamp 0.9 instead of 2, 3×3 blur, no history
   length). Fixed, and **C4838 (narrowing conversion in an initializer list) is now a compile error**.
4. **Sun and occlusion**: the sun's share came from the game's shadow map, which does not match in Red Dead (v44), and
   from the sun / sky ratio measured with it (0.04 in full sun: sun "too weak", cut). Sunlit walls darkened as if in
   shadow. Fixed: **an RT ray toward the sun** (leaf holes crossed) decides whether the point is lit (`ao_sunray=1`); an
   implausible sun / sky ratio (< 0.3) → 2.2; `sun_measure=0` in Red Dead.

**Techniques added (research: NRD / ReBLUR, NVIDIA patents on bounded history, spatio-temporal blue noise)**
- **Dual history** (`denoise_dual=1`): a fast history (2-3 frames, at least half new) stored with the history length;
  the slow history is clamped around it (window 0.5 σ + 0.04, tightened in motion), as well as around the noisy current
  neighbourhood.
- **Low-discrepancy rays** (`ao_ign=1`): interleaved gradient noise shifted every frame, shared rotation and azimuth
  strata, golden ratio between a pixel's rays (contact and long rays).

**Measurements (Red Dead, tree-lined street and park in Saint Denis, in the sun)**
- v70 / v83 / v105: instability at rest 0.19 / 0.20 / **0.11**; while turning 0.22-0.50 / 0.43 / **0.20-0.23**; follow
  0.90-0.91 / 0.91-0.95 / **0.94-0.95**; fixed offset 0.02 / 0.02 / **0.00-0.02 px**; share darkened by more than 5 %
  18-21 % / 23-27 % / 24-27 %; added flicker at rest 0.39 / 0.66 / 0.40 %, walking 0.09-1.04 / 0.26-1.15 / 0.30-0.67 %.
- Sunlit walls: only the sky's share removed; sun patches on the ground intact, tree shadows darkened.

## v106 — URTI: characters no longer cast ambient shadow, checkerboard occlusion in motion (performance)

- Player feedback (captures in a Saint Denis alley): dark, blurry "shadow drawings" stay stuck to the walls and follow the
  screen; the added shadows cost too many frames per second.
- Likely cause of the spots: the player's character is in the RT scene; standing against a wall, it hides the "sky" of
  the long rays (16 m) from a large area of the wall, and that spot follows it, hence follows the screen. Fixed: a hit
  point that, projected on screen, falls on a character (alpha of Red Dead's G-buffer normals) at about the same depth is
  crossed, like a leaf hole (occlusion, long rays, sun ray).
- Performance: in v104, a moving pixel was computed every frame; when the camera moves, almost the whole screen is, and
  the pass's cost was multiplied by up to four. Now every other frame, in a checkerboard (same cost per 2 × 2 block as at
  rest). Occlusion regulator target `ao_ms` 4 → 2.5 ms.
- Installed in Red Dead at the player's request, who tests it themself; not yet measured by URTI. Field of view: during
  their session, measured 55-69° and corrected to 56.1° (safeguards respected: still camera, stable measurement); Red Dead
  probably changes its field depending on the camera (alley, mode).

## v107 — URTI: reflections only, full documentation, repository published

- Player's decision: occlusion ("the shadow system") is too unstable in game; only reflections are kept, "already
  exceptional". "Occlusion" checkbox unticked by default in urti.fx (the code stays, `UrtiOcclusion` hidden). Sun tracking
  stays active in a game without a read camera: its cascades measure the field of view, which reflections need (Red Dead).
  Stored field of view reset to 37.92°.
- Documentation: `README.md` rewritten (current state: reflections, glossy floors, a single checkbox, per-game settings,
  measurement tools) and `docs/HOW_IT_WORKS.md` (the whole chain: capture, RT scene, measurements, materials, rays,
  stabilisation, composition, optional occlusion, measurement method, lessons, limits).

## v108 — URTI: Red Dead back to sharp reflections (no glossy floors or terrain)

- Player feedback: reflections seem lower quality than before. In-game comparison (`fov_force=` added to impose the field
  of view): the character changed place during the session, so no clean comparison; but the bursts show the glossy floors
  (v99-v101) giving all the dry pavement a blurred, shiny reflection, looking wet and smeared, where before there were
  only sharp mirrors. Field of view 38°: more surfaces pass the flatness tests (21.5 / 11.5 % of the screen with a
  reflection against 9.8 / 7.7 % at 50°, same moment).
- Red Dead's settings put back to those of the earlier reflections: no more `gloss_min`, `gloss`, `tess`, `gnormal_*`
  (copy `urti.ini.avant_v108`). Measured field of view kept (correct angles). If the old look at 50° was preferred:
  `fov_force=50`.
- README images replaced by three of the player's v108 captures (parquet hall, puddle in front of a house, alley with wet
  flagstones); the old before / after images removed.

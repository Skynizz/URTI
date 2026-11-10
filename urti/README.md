# URTI add-on

The ReShade add-on and its test bench. It observes a DirectX 12 game, builds a parallel scene and casts rays
into it, without modifying the game's shaders. Feasibility notes: [docs/URTI.md](../docs/URTI.md) (in French).

## Status

| Step | Content | State |
|---|---|---|
| 0 | Bench: rasterised DX12 app with known geometry (`testapp/`), hidden window | done |
| 1 | Geometry capture through stream output, coverage test | **99.80 %** on the bench; **97.5 to 97.6 % in Red Dead Redemption 2** (DX12, DLSS + frame generation, 3 captures) |
| 2a | Camera estimated by self-calibration (`tools/analyse.py`) | bench: 60.15 degrees for 60.16; RDR2: 36.5 to 37 degrees while the camera turns |
| 2b | Persistent world-space geometry, BVH, RT sun shadows | done |
| 3 | Radiance cache, reflections and indirect lighting | done |

## How the capture works (ReShade add-on, `addon/urti_addon.cpp`)

- Each graphics pipeline of the game gets a "shadow" pipeline: same vertex shader and root signature, stream
  output of `SV_Position`, no rasterisation or pixel shader.
- During a captured frame, each draw is replayed once with this pipeline, then the game's state is restored:
  nothing changes on screen. This yields triangles as the game transformed them (GPU animation, instancing,
  compressed vertices included), with nothing to decode.
- D3D12 requires `ALLOW_STREAM_OUTPUT` in the root signature: the add-on adds it to the game's root signatures
  at creation (`ID3D12Device::CreateRootSignature`).
- Coverage test: the captured triangles, rasterised again, must reproduce the game's depth. No camera is needed
  for this test.

What a real game (RDR2) required:
- only use the D3D12 device that presents the image (the game creates several: crash otherwise);
- one shared stream per screen-sized depth, draws stored one after the other (barrier on the counter between
  two draws, otherwise they overwrite each other); shadow maps left aside;
- read depth from ReShade's copy (invisible `reshade/urti_depth.fx` effect): RDR2 neither clears it with
  `ClearDepthStencilView` nor through a render pass;
- capture over 4 presents (with frame generation, one out of two has no game draw);
- only compare pipelines that write depth (full-screen lighting passes read it).

Limits: pipelines with geometry / tessellation / mesh shaders are ignored; triangles crossing the near plane are
ignored in the coverage test.

## Build and test

    cmake -S urti -B urti/build -A x64 && cmake --build urti/build --config Release
    # in urti/build/Release: ReShade 6.8.0 "Addon" renamed dxgi.dll, ReShade.ini, urti.addon64
    URTI_FRAMES=90 URTI_CAPTURE_FRAME=60 ./urti_testapp.exe   # log: urti_log.txt

Third-party headers: `third_party/reshade` (ReShade 6.8.0 add-on API, BSD-3), `third_party/imgui`.
Games: single player only (the "Addon" build of ReShade is refused online).

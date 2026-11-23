# URTI: plan and feasibility assessment (October 2026)

Skynizz's idea: an intermediate layer that watches a DirectX 12 game without ray tracing, builds a parallel and
persistent geometric representation (temporal geometry cache; static / rigid / deformed / unknown classes; confidence
levels), and adds RT shadows, reflections and then indirect lighting, composed with the original image.

Assessment: feasible as a prototype on one game, in steps with stop criteria. "Universal and robust" is not realistic
without a small per-game profile (like RTX Remix). Risks, from most to least serious:
1. World-space geometry: do not decode the buffers, capture the vertex shaders' output (stream output) instead. The
   camera still has to be found. Mesh shaders escape this.
2. Composition: the game's own shadows and reflections cannot be removed from the final image. Without stepping into
   its lighting pass, RT can only add.
3. Off-screen materials: rather than decoding them, keep the light of the surfaces seen in a world-space cache (KRC)
   and read it back where rays land.
4. Lights: the sun from the shadow-map passes; the others later.
5. Anti-cheat: single player only.

Steps: 0, a bench with ground truth; 1, capture and coverage (> 95 % of the game's depth explained, otherwise stop);
2, RT sun shadows; 3, light cache, reflections, indirect. Tracking: urti/README.md.

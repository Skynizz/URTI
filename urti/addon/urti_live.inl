// Live URTI (included in urti_addon.cpp): ray-traced effects on every frame.
//
// Every frame: the draws on the screen depth are replayed into the stream (like a
// capture); before ReShade's effects, on its command list: vertices to camera space,
// acceleration structure, rays from each pixel, effect layers read by the urti.fx effect.
// The depth law and the sun's shadow map come from a calibration (full capture,
// shadow maps included) done at activation and on F6; afterwards the sun direction is
// measured every frame on the GPU (pairs of draws screen / shadow map), without readback.
//
// Keys: F7 on / off; F6 recalibrates. File urti_photo next to the add-on: image of
// the screen after the effects (urti_photoN.ppm).

#include "live_convert_cs.h" // g_live_convert
#include "live_shadow_cs.h"  // g_live_shadow
#include "live_sunfit_cs.h"  // g_live_sunfit
#include "live_sunvote_cs.h" // g_live_sunvote
#include "live_composite_cs.h" // g_live_composite
#include "live_reflect_cs.h"   // g_live_reflect
#include "live_depthfit_cs.h"  // g_live_depthfit
#include "live_mvcal_cs.h"     // g_live_mvcal
#include "live_reflresolve_cs.h" // g_live_reflresolve
#include "live_shadowtemporal_cs.h" // g_live_shadowtemporal
#include "live_regionstats_cs.h" // g_live_regionstats
#include "live_uicompare_cs.h" // g_live_uicompare
#include "live_uiselect_cs.h" // g_live_uiselect
#include "live_hudless_cs.h" // g_live_hudless
#include "live_hudgain_cs.h" // g_live_hudgain
#include "live_film_cs.h"    // g_live_film
#include "live_waterbase_cs.h" // g_live_waterbase
#include "live_rotfit_cs.h" // g_live_rotfit
#include "live_radcache_cs.h" // g_live_radcache
#include "live_delay_cs.h" // g_live_delay
#include "live_sunlight_cs.h" // g_live_sunlight
#include "live_counts_cs.h" // g_live_counts
#include "live_surfcache_cs.h" // g_live_surfcache
#include "live_camfit_cs.h" // g_live_camfit
#include "live_mvsynth_cs.h" // g_live_mvsynth
#include "live_planes_cs.h" // g_live_planes
#include "live_geoconvert_cs.h" // g_live_geoconvert
#include "live_geoinst_cs.h" // g_live_geoinst
#include "live_refpairs_cs.h" // g_live_refpairs
#include "live_objplanes_cs.h" // g_live_objplanes

struct LiveState
{
    bool enabled = false, calibrated = false, bound = false;
    bool depthReady = false; // depth law known (reflections); calibrated: sun known (shadows)
    // stored calibration (urti_calib.ini): per game, read back at launch -> effects from the first frame
    bool fromStore = false, verified = false, storeLoaded = false;
    uint32_t sunMapWidth = 0, sunMapHeight = 0, sunMapFormat = 0;
    float sun[3] = { 0, 1, 0 }, A = 0, B = 0.1f, p00 = 1, p11 = 1;
    uint64_t nextCalibration = 0;
    uint32_t calibrationInterval = 180;

    ComPtr<ID3D12RootSignature> convertRs, shadowRs;
    ComPtr<ID3D12PipelineState> convertPso, shadowPso;
    ComPtr<ID3D12Resource> view, blas, blasScratch, tlas, tlasScratch, instances, depthBuf, shadowBuf, mask, counterReadback[3];
    ComPtr<ID3D12Resource> prevShadowBuf; // mask of the previous real frame (generated frames: blend)
    ComPtr<ID3D12Resource> shadowRawBuf;  // raw mask of this frame (before denoising)
    ComPtr<ID3D12RootSignature> denoiseRs;
    ComPtr<ID3D12PipelineState> denoisePso;
    // geometry budget: each captured object is measured (distance, size); only the most important
    // enter the RT scene, up to vertexBudget vertices; the others are re-measured in rotation
    // (probeBudget vertices per frame, outside the structure)
    // snapshot before the UI: screen snapshots (urti_addon.cpp), comparison, choice
    ComPtr<ID3D12Resource> snaps[kLiveSnaps], uiCounts, uiReadback[3], preUiBuf;
    ComPtr<ID3D12RootSignature> uiRs, uiSelRs;
    ComPtr<ID3D12PipelineState> uiPso, uiSelPso;
    uint64_t snapBytes = 0, uiFrame = 0;
    uint32_t uiCountAt[3] = {};
    int uiChoice = -1; // rank of the snapshot before the UI (-1: none safe), read three frames later
    uint32_t uiParity[3] = {};
    float uiReflGate = 1.0f; // RT reflections faded out smoothly when no snapshot is safe (otherwise the UI is reflected)
    uint32_t uiBadReadbacks = 0; // stale snapshots in a row (read three frames later)
    float uiZone[4] = {}; // urti.ini ui_zone=x0,y0,x1,y1 (fractions of the screen): UI zone never touched
    ComPtr<ID3D12Resource> uiMaskBuf; // UI pixels kept from the last safe snapshot (live_composite)
    uint64_t uiMaskBytes = 0;
    bool uiPhoto = true; // urti.ini ui_photo=0: without the snapshot before the UI (control)
    // the game's image without its UI, copied inside its lists (urti.ini hudless=0: without), put back into the screen's layout
    static constexpr uint32_t kHudSnaps = 4;
    ComPtr<ID3D12Resource> hudSnap[kHudSnaps];
    uint64_t hudBytes = 0;
    uint32_t hudPitch = 0, hudWidth = 0, hudHeight = 0, statHudless = 0, statHudConverted = 0, statShifted = 0, statHudMatched = 0;
    double statHudMatchSum = 0;
    uint32_t statHudLag[3] = {}, statHudConvBack[4] = {};
    // layer queue (live_delay): three slices, each computed frame's layer stored with its number
    ComPtr<ID3D12Resource> delayRing;
    ComPtr<ID3D12RootSignature> delayRs;
    ComPtr<ID3D12PipelineState> delayPso;
    uint32_t delayW = 0, delayH = 0;
    LARGE_INTEGER lastPresentTime = {};
    double presentMs = 0;
    uint32_t suspectLeft = 0;
    double presentDrawsAvg = 0;
    bool layerSuspect = false;
    uint32_t statSuspect = 0;
    uint64_t layerHudSerial = 0;   // number of the last copy of the UI-free image at the time of the last computation
    float lagBias = 0;             // urti.ini lag_bias=: correction of the layer's lead (game frames)
    bool lagOn = true, hudLeadNow = false; // urti.ini lag=0: layer laid as it is (old rule)
    double statLeadSum = 0;
    float hudSlopeNow = 0;
    uint64_t hudConverted = ~0ull; // number of the copy put back into the screen's layout (preUiBuf, mask); ~0: none
    bool hudlessOn = true;
    uint32_t hudlessMode = 1; // urti.ini hudless=2: conversion done but not used (to measure its cost)
    ComPtr<ID3D12RootSignature> hudRs, hudGainRs;
    ComPtr<ID3D12PipelineState> hudPso, hudGainPso;
    ComPtr<ID3D12Resource> hudGain, hudGainReadback[3]; // final image / UI-free image ratio (live_hudgain), read for the log
    ComPtr<ID3D12RootSignature> statsRs;
    ComPtr<ID3D12PipelineState> statsPso;
    ComPtr<ID3D12Resource> statsBuf, statsUpload[3], statsReadback[3];
    uint64_t statsBytes = 0;
    std::vector<uint32_t> statsIds[3], statsCaps[3]; // measured identities and their reserved room (vertices)
    std::unordered_map<uint32_t, uint32_t> idSig;        // ExecuteIndirect: identity -> command signature
    std::unordered_map<uint32_t, float> sigVertices;     // signature -> largest number of vertices written (decays)
    struct ObjectStat
    {
        float importance;  // size / distance (apparent size)
        uint64_t frame;    // frame of the last measurement
        uint32_t vertices; // vertices really written (measured)
        float minZ;        // closest distance to the camera
    };
    std::unordered_map<uint32_t, ObjectStat> objects;
    std::unordered_map<uint32_t, uint32_t> seenNow;
    std::unordered_set<uint32_t> admitted;
    uint32_t churnIn = 0, churnOut = 0; // objects that entered / left the RT scene this frame
    uint32_t burst = 0, burstIndex = 0, burstCount = 0; // burst of frames before / after URTI in progress (frames left)
    // real-pace film (urti_film file): every present, before and after URTI, at half resolution, kept in video memory
    // until the end, then written (urti_film_N\before_k.raw, after_k.raw); one FILM line per present
    static constexpr uint32_t kFilmFrames = 30;
    uint32_t film = 0, filmIndex = 0, filmCount = 0;
    std::string filmFolder;
    ComPtr<ID3D12Resource> filmRing;
    uint64_t filmFrameBytes = 0;
    uint32_t filmWidth = 0, filmHeight = 0, filmFormat = 0;
    ComPtr<ID3D12RootSignature> filmRs;
    ComPtr<ID3D12PipelineState> filmPso;
    std::string burstFolder;
    uint64_t statChurn = 0; // objects that entered or left the RT scene (summary every 300 frames)
    bool composedNow = false;           // URTI's composition happened on this present
    std::atomic<bool> showGameOnly{ false }; // F4: the game's image alone (full-screen before / after comparison)
    int menuOn = -1; // "URTI" menu checkbox (UrtiOn) as read on the last present; -1: not read yet
    uint64_t vertexBudget = 6000000, probeBudget = 1000000, slotLimit = 0;
    uint64_t statDraws = 0, statOverflow = 0, statOutOfBudget = 0, statProbes = 0, statBudgetFrames = 0, statReserved = 0;
    // accumulation over time: the game's motion vectors (G-buffer), previous depth
    ComPtr<ID3D12Resource> mvBuf, depthPrevBuf, mvErrors, mvErrorsReadback[3];
    ComPtr<ID3D12RootSignature> mvcalRs;
    ComPtr<ID3D12PipelineState> mvcalPso;
    uint64_t mvBytes = 0, mvFrame = ~0ull;
    uint32_t mvPitch = 0, mvWidth = 0, mvHeight = 0, mvConvention = 255, mvMovingFrames = 0;
    // target tried among the G-buffer's two-half-float targets (some games have two, the first
    // almost empty: every convention stayed tied there); measurements without a winner on this one
    uint32_t mvCandidate = 0, mvUndecided = 0;
    // camera motion measured on our geometry (live_camfit) and motion vectors built from it
    // (live_mvsynth) when the game's stay unreadable; urti.ini motion_source= 0: automatic choice, 1: the
    // game's only, 2: ours
    ComPtr<ID3D12RootSignature> camfitRs, mvsynthRs;
    ComPtr<ID3D12PipelineState> camfitPso, mvsynthPso;
    ComPtr<ID3D12Resource> pairsBuf, motionBuf, motionReadback[3];
    bool mvSynthetic = false;
    // layer of flat transparent surfaces (water, glass: reflections); state of its vertex buffer
    ComPtr<ID3D12RootSignature> planesRs;
    ComPtr<ID3D12PipelineState> planesPso;
    ComPtr<ID3D12Resource> viewT, blasT, blasTScratch, transpUpload[3], transpSrcUpload[3], planeStats, planeReadback[3];
    bool viewTRead = false, transpBuilt = false, waterOn = false;
    uint32_t transpMiss = 0; // windows in a row without a transparent surface (game frame split between two presents)
    // geometry cache: each object outside the frame's budget, captured in turn, has its structure kept in a
    // reserve (ring buffer), placed in the top-level structure with the camera's pose (live_camfit)
    struct GeoEntry
    {
        uint64_t offset = 0, size = 0, frame = 0;
        uint32_t triangles = 0;
        uint32_t slot = ~0u, gen = 0; // place in the table of learned colours (live_surfcache), generation of that place
    };
    // learned colours of cached objects: 256 per object (one group of neighbouring triangles each), seen on screen when the object is; a reflection that
    // hits it hidden or off screen takes them instead of staying unknown (the game's blocky reflection showed underneath otherwise)
    static constexpr uint32_t kGeoColorBins = 256;
    std::vector<uint32_t> geoSlotOwner;
    std::vector<uint8_t> geoSlotGen;
    uint32_t geoSlotNext = 0;
    ComPtr<ID3D12Resource> geoColors;
    std::unordered_map<uint32_t, GeoEntry> geo;
    std::map<uint64_t, uint32_t> geoByOffset;
    std::unordered_map<uint32_t, uint64_t> meshProbed; // last round of each mesh
    // witnesses: small objects in place, far from the camera, captured every frame: the camera's motion is measured
    // on them (the frame's scene only keeps what is near: the character, whom the camera follows)
    std::unordered_set<uint32_t> stableMeshes, sentinels;
    // their reference points in the cache's frame (live_refpairs): place of each witness, free places
    std::unordered_map<uint32_t, uint32_t> sentinelRef;
    std::vector<uint32_t> sentinelFree;
    ComPtr<ID3D12Resource> sentRefs, sentUpload[3];
    ComPtr<ID3D12RootSignature> refpairsRs, objplanesRs;
    ComPtr<ID3D12PipelineState> refpairsPso, objplanesPso;
    // flatness of each captured object (live_objplanes), read three frames later: what may be water
    ComPtr<ID3D12Resource> objPlanes, objPlanesRead[3], objPlanesUpload[3];
    std::vector<uint32_t> objPlanesIds[3];
    uint64_t objPlanesBytes = 0;
    std::unordered_set<uint32_t> flatLogged;
    uint64_t motPairs = 0;
    bool motionLog = false; // urti.ini motion_log=1: motion measured every frame (log)
    uint32_t reflectDiag = 0; // urti.ini reflect_diag=: view 7 = reflected ray direction, normal
    int depthLawFixed = -1;   // urti.ini depth_law_fixed=: -1 the known law as soon as there is one, 0 the measurement, 1 the known law
    float diagReflStrength = 0; // values of the last composition (BURST lines of the log)
    uint32_t diagDebugReflect = 0, diagSnaps = 0;
    int diagHistOut = -1;
    bool diagPreUi = false;
    ComPtr<ID3D12Resource> lawUpload[3];
    // fence between the game's queues and the present queue: per game queue, its fence and its last value
    struct QueueSync
    {
        ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
    };
    std::unordered_map<uint64_t, QueueSync> queueSync;
    bool queueSyncOn = true;
    uint64_t statSyncWaits = 0;
    // reserved vertices of each mesh, kept for a few windows: a capture window sometimes holds only part
    // of a game frame, and a mesh not seen has not left the screen
    struct SeenMesh
    {
        uint32_t vertices = 0;
        uint64_t frame = 0;
    };
    std::unordered_map<uint32_t, SeenMesh> seenRecent;
    ComPtr<ID3D12Resource> geoPool, geoView, geoScratch, geoInstances, geoUpload[3], geoRegionUpload[3];
    ComPtr<ID3D12RootSignature> geoconvRs, geoinstRs;
    ComPtr<ID3D12PipelineState> geoconvPso, geoinstPso;
    bool geoOn = true, geoResetPose = true;
    // the game's camera read from its memory (urti_camscan.inl): exact pose, without measuring the geometry. camLive: at least one
    // camera seen; camCur: the one of the frame being computed; camRef: the one of the geometry cache's fixed frame
    bool cameraOn = true, camLive = false, camHaveCur = false, camHaveRef = false;
    uint32_t camSerialNow = 0, camSerialDone = 0, statCamFrames = 0, statCamNew = 0, statCamJumps = 0;
    uint32_t camSeenSerial = 0;
    uint64_t camSeenFrame = 0;
    bool camLost = false;
    GameCamera camCur, camRef;
    double statCamTrans = 0, statCamRot = 0, statCamTransMax = 0;
    ComPtr<ID3D12Resource> camUpload[3], motionCheck;
    uint64_t geoHead = 0, geoPoolBytes = 0, geoWantBytes = 1000ull * 1048576, geoLastFlush = 0;
    uint64_t statGeoFrames = 0, statGeoBuilds = 0, statGeoVerts = 0, statGeoEvicted = 0, statGeoNear = 0;
    uint32_t statGeoFlushes = 0, statGeoBadFit = 0;
    double geoPoseDistance = 0.0;
    uint64_t planeFrames = 0, planeKept = 0, planeTris = 0, planeSeen = 0;
    uint32_t mvGiveUps = 0, motionSource = 0, motMovingNoGame = 0; // frames where the camera moved without a game convention found
    double motRot = 0, motTrans = 0, motKept = 0, motRms = 0;
    uint32_t motFrames = 0, motValid = 0;
    double mvErr[16] = {}, mvCount[16] = {};
    float temporalAlpha = 0.3f;
    float reflAlphaMin = 0.1f, reflAlphaMax = 0.5f; // reflections: share of the current frame, at rest and in fast motion
    float reflGamma = 1.0f, reflDepthTol = 0.1f;    // reflections: clamp width (standard deviations), depth tolerance
    uint32_t debugView = 0;    // diagnostic view (UrtiDebug menu or urti.ini debug_view=)
    int iniDebugView = -1;
    // stabilised reflections: history at render resolution (two alternating buffers)
    ComPtr<ID3D12RootSignature> resolveRs;
    ComPtr<ID3D12PipelineState> resolvePso;
    ComPtr<ID3D12Resource> reflHist[2];
    uint64_t reflHistBytes = 0;
    int reflHistLast = -1; // buffer written on the last real frame
    uint32_t fgMode = 0;                  // generated frames: 0 same mask, 1 without RT, 2 blend of the last two
    // generated frames: the last computation's shadow and reflection slide by this share of one frame's motion (urti.ini fg_shift=, 0: frozen)
    float fgShift = 0.5f;
    // geometry cache: vertices built per frame, new objects first (urti.ini geo_new=), objects already kept refreshed afterwards
    // (urti.ini geo_refresh=); before: up to 2 million, 80 objects and 400,000 vertices rebuilt every frame (1.9 ms)
    uint32_t geoNewVerts = 300000, geoRefreshVerts = 100000;
    // occlusion: more rays where the image moves (urti.ini ao_boost=1); off: while walking, the whole screen moves and the pass cost
    // half as much again
    bool aoBoost = false;
    // time budget per pass, held frame by frame: the level rises as soon as the pass exceeds its target (rays, hops, range, pixels processed) and comes down slowly.
    // urti.ini ao_ms= (occlusion and denoising), refl_ms= (reflections), 0: no budget
    int aoLevel = 0, reflLevel = 0;
    uint32_t aoCool = 0, reflCool = 0;
    float reflTarget = 3.5f;
    // smoothed water colour under the reflection (urti.ini water_base=0: off), only with the game camera (Horizon: its low-resolution reflection is in the water)
    float waterBaseOn = 0.0f, waterGain = 1.5f; // urti.ini water_gain=: weight of the reflection on the water
    ComPtr<ID3D12Resource> waterBase[2];
    ComPtr<ID3D12RootSignature> waterBaseRs;
    ComPtr<ID3D12PipelineState> waterBasePso;
    uint32_t waterBaseW = 0, waterBaseH = 0;
    uint64_t waterBaseFrame = ~0ull;
    double aoEma = 0, reflEma = 0, statAoMax = 0, statReflMax = 0;
    double aoHist[9] = {}, reflHist9[9] = {};
    uint32_t aoHistN = 0, reflHistN = 0;
    uint32_t statAoSpikes = 0, statReflSpikes = 0, statAoLevelSum = 0, statReflLevelSum = 0;
    float aoDistNow = 50.0f, reflTMaxNow = 500.0f;
    uint32_t reflStrideNow = 2, reflHopsNow = 3, aoHopsNow = 2;
    ComPtr<ID3D12Resource> denoiseLen[2]; // history length per pixel (live_shadowtemporal), every other frame each
    uint32_t denoiseLenNext = 0;
    float reflSpatial = 0.5f; // urti.ini refl_spatial=: share of the neighbours' average in the current reflection (0: old rule)
    bool reflCoherent = true; // urti.ini refl_coherent=0: reflections on isolated smooth pixels too (old rule)
    float reflSoft = 1.0f;    // urti.ini refl_soft=: radius (pixels) of the area where the reflected colour is read (0: a point, old rule)
    bool denoiseLenOn = true; // urti.ini denoise_len=0: old rule (no history length)
    bool reflFull = true;  // urti.ini refl_full=0
    bool denoiseFarSkip = true; // urti.ini denoise_far=0: denoiser on every pixel (old rule)
    bool sunEighth = false; // urti.ini sun_eighth=1: sun map captured in eighths when sun shadows are off (test: 1.4 -> 0.6 M vertices replayed, gain not measurable)
    uint64_t geoProbe = 2000000; // urti.ini geo_probe=: per-frame cap of the measurement area when the geometry cache is active (vertices replayed): one reflection ray per pixel while the budget holds (old rule)
    bool waterOnly = true;     // urti.ini reflect_all=1: reflections also on smooth surfaces that are not water
    bool reflStatsOn = false;  // urti.ini reflect_stats=1: reflecting pixels and rays counted
    ComPtr<ID3D12Resource> reflStats, reflStatsRb[3];
    uint64_t statRs[8] = {};
    uint32_t statRsFrames = 0;
    bool slowRays = false; // urti.ini rayons_lents=1: every triangle met is examined (old rule, to compare the cost)
    // world radiance cache (live_radcache, live_radhash.hlsli; game camera known): spatial hash table of the colours seen on screen,
    // sky and distance map; reflMix: per render pixel, colour of the reflection already known and share to read from the image
    bool radOn = true;          // urti.ini radcache=0: colours learned per object (old rule)
    float radVoxel = 0.005f;    // urti.ini rad_voxel=: size of a cell, as a share of the distance
    uint32_t radBits = 23;      // 2^23 cells of 16 bytes (128 MB)
    uint32_t radEnvSize = 512;
    ComPtr<ID3D12Resource> radHash, radEnv, reflMix, radUpload[3], radStats, radStatsRb[3];
    ComPtr<ID3D12RootSignature> radRs;
    ComPtr<ID3D12PipelineState> radPso;
    ID3D12Resource* radParamsNow = nullptr; // settings of the computed frame (radUpload ring), read again at composition
    uint64_t radStamp = ~0ull, reflMixBytes = 0, radStatFrames = 0, statRad[7] = {};
    bool reflMixValid = false;  // the last reflection computation wrote reflMix
    int worldUpAxis = 2;
    float worldUpSign = 1.0f;
    LARGE_INTEGER radClock0 = {};
    ComPtr<ID3D12Resource> reflMotion; // live_reflresolve -> live_composite: motion of the surface and of the reflection per render pixel
    uint64_t reflMotionBytes = 0, reflMotionFrame = ~0ull;
    // sun stability: difference from one measured frame to the next
    float lastSun[3] = {};
    double sunJitterSum = 0, sunJitterMax = 0;
    uint32_t sunJitterCount = 0;
    uint64_t capacity = 0; // vertices of the structure
    uint32_t width = 0, height = 0, depthPitch = 0, shadowPitch = 0; // width, height: rendered area (render resolution)
    uint32_t allocWidth = 0, allocHeight = 0; // size of the depth (images allocated at that size)
    float viewMax[2] = {};                    // the game's measured viewport (last window)
    uint32_t viewLogCount = 0, viewCandidate = 0, viewStreak = 0;
    bool renderAreaOn = true; // urti.ini render_area=0: the whole depth (test)
    bool aoRough = false;     // urti.ini ao_rough=1: occlusion also on rough surfaces (grass), overall slope
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    resource_view maskSrv = {};
    uint64_t frame = 0, lastFilled = 0;
    // acceleration structure: updated (refit, several times cheaper) as long as the list of
    // draws is the same as at the last build, rebuilt at least every blasRefit frames
    uint64_t blasLayout = 0;
    // blasRefit: at most this many updates in a row; 0 by default: in RDR2 a single update made reflection
    // rays fourteen times slower (vegetation batches sorted by the GPU: triangles change places)
    uint32_t blasAge = ~0u, blasRefit = 0, statRebuilds = 0, statUpdates = 0;
    uint64_t statVertices = 0;
    std::vector<ComPtr<ID3D12Resource>> retired[3]; // released three frames later
    uint32_t photoIndex = 0;
    // geometric memory: vertices of the previous frame (in that frame's camera space) and, per
    // object identity, where its region started; a hit point thus finds its previous position,
    // deformations included (wind, animation: vertices are captured after the vertex shader)
    ComPtr<ID3D12Resource> viewPrev, linkUpload[3], anchor[2];
    std::unordered_map<uint32_t, uint32_t> prevStart, pendingStart;
    bool prevValid = false;
    int anchorOut = -1; // anchoring written this frame (anchor[anchorOut]), the other is the previous frame's
    uint64_t anchorFrame = ~0ull;
    bool anchorPrevOk = false; // the other buffer was indeed written on the previous frame
    uint64_t anchorBytes = 0;
    // anchoring off by default: measured on the sandbox, it does not reduce blinking (switching from one
    // filter to the other depending on the pixel adds some); anchor_alpha=0.1 in urti.ini to try it
    float anchorAlpha = 0.0f, anchorGamma = 2.5f;
    bool alphaScreen = true; // holes of cut-out cards recognised on screen (urti.ini alpha_screen=0: off)
    // shadows denoised by NVIDIA's SIGMA (urti.ini nrd=1) if the add-on has NRD; off by default as long as it does not beat
    // URTI's denoising on the sandbox (it lacks the game's sub-pixel jitter, unknown for now)
    bool nrdShadows = false;
    uint32_t statNrd = 0;
    float nrdDisocclusion = 0.01f, nrdFrames = 5.0f;
    bool jitterLog = false;
    float fovStretch = 0; // t² measured by live_sunvote (stretch of the assumed projection)
    bool fovAuto = true;  // projection corrected by the measured field of view (urti.ini fov_auto=0: assumed 50°)
    bool fovReset = false; // the measurement's sums to reset (projection changed)
    // measured field of view (corrected at least once, or read back from urti_calib.ini): calibration no longer puts back the assumed 50°. From v95 to v105,
    // every sun search in Red Dead put them back, and the correction never ran again (rotation threshold too high): URTI worked at 50°
    // for a game at 38°, wrong angles (floor normal, reflection direction, occlusion shape: "shifted" reflections)
    bool fovMeasured = false;
    float fovK2Ref = 0; // reference measurement (stability over time)
    uint64_t motLastMoveFrame = 0; // last frame where the camera moved forward (our measurement on the geometry)
    uint64_t fovK2RefFrame = 0;
    float fovStored = 0; // vertical field read back (degrees), 0: none
    float fovForce = 0;  // fov_force=: imposed vertical field (degrees, comparisons), 0: measured
    uint32_t fovHold = 0;  // frames to ignore after a correction (measurements read three frames late)
    // sun share in the game's image (live_sunlight): sun / sky ratio per channel (red, green, blue,
    // luminance), smoothed; RT shadows remove only that share (urti.ini sun_measure=0: fixed slope)
    bool sunMeasure = true;
    uint32_t gameDilate = 1; // urti.ini game_dilate=: neighbourhood read in the game's map (radius in texels)
    bool gameMin = true;
    float aoAmount = 0.8f, aoRadius = 1.5f; // RT ambient occlusion (urti.ini ao=, ao_radius=; ao=0: off)
    // RT sun shadows: off by default (urti.ini shadows=1): in RDR2, its already excellent shadows leave
    // almost none to add (burst in the sun: 0.0 % of the image changed for 3.2 ms per frame)
    bool sunShadows = false;
    bool aoQuarter = true; // urti.ini ao_quarter=0: occlusion computed on every pixel every frame
    // occlusion rays per computed pixel (quarter rate), from 2 to 4, tuned every 30 frames on the measured time
    // of the shadow pass (urti.ini ao_ms=, 0: always 4)
    float aoTarget = 4.0f;
    uint32_t aoRays = 4;
    double shadowMs = 0;
    // geometry budget driven by the measured rebuild cost (urti.ini build_ms=, 0: fixed budget):
    // above the target, it drops (objects with the smallest apparent size leave first), below it
    // rises again up to vertex_budget
    float buildTarget = 2.5f;
    uint64_t budgetMax = 6000000;     // urti.ini game_min=0: game shadow widened (all or nothing) and product of visibilities
    float sunSky[4] = { 2.2f, 2.2f, 2.2f, 2.2f };
    uint32_t sunSkyFrames = 0, sunSkyLogged = 0;
    bool sunWeakLogged = false;
    uint64_t sunSkyPairs = 0, atlasFrame = ~0ull;
    uint64_t mapCheck[6] = {}; // check of the game's map: lit points, shadowed, on the edge, outside the cascades; luminances (× 1024)
    uint64_t depthHist[16] = {}, gapHist[16] = {}; // depth of the points in the cascade, map - point difference
    ComPtr<ID3D12RootSignature> sunlightRs;
    ComPtr<ID3D12PipelineState> sunlightPso;
    uint32_t jitterMode = 2; // the game's sub-pixel jitter compensated: 0 no, 1 frame-to-frame difference, 2 the absolute value too // SIGMA: disocclusion threshold, stabilised frames
    uint64_t boundFrame = 0;

    // sun tracking every frame
    bool forceCalibration = false, sunDirty = false;
    int iniCombine = -1; // urti.ini combine=: 0 or 1 imposes the mode; -1: ReShade menu (UrtiMode)
    std::atomic<bool> overlayOpen{ false }; // ReShade menu open: no RT pass (the menu must stay visible)
    uint32_t shadowRays = 4;                // urti.ini shadow_rays= (1 to 4)
    // GPU timing per step (timestamps), averaged over 300 frames
    ComPtr<ID3D12QueryHeap> timeHeap;
    ComPtr<ID3D12Resource> timeReadback[3];
    double timeSum[8] = {};
    // detailed timing ("GPU detail" log): computation, markers 0 to 11 (once per computation); composition, markers 12 to 17 (on every
    // present, its own counter)
    static constexpr uint32_t kTimeD = 24;
    ComPtr<ID3D12QueryHeap> timeDHeap;
    ComPtr<ID3D12Resource> timeDRb[3], timeCRb[3];
    double timeDSum[kTimeD] = {};
    uint32_t timeDFrames = 0, timeCFrames = 0;
    uint64_t compCounter = 0;
    uint32_t timeFrames = 0;
    uint32_t lastPresentDraws = 1; // game draws executed before this present (0: nothing new to compute)
    int lastMainSlot = -2;         // main screen of the last computed frame (logged when it changes)
    uint32_t lastDrawsThisFrame = 0;
    // recent presents where the game had drawn into the presented image (bit 0: this one); presented image
    // made by frame generation (not by the game)
    uint32_t backBufferDrawn = 0, presentNoDraw = 0; // and recent presents without any game draw executed
    bool imageGenerated = false;
    // reflections: G-buffer material copied every frame, reflected rays
    ComPtr<ID3D12RootSignature> reflectRs, depthfitRs;
    ComPtr<ID3D12PipelineState> reflectPso, depthfitPso;
    ComPtr<ID3D12Resource> materialBuf, reflectBuf;
    uint64_t materialBytes = 0, materialFrame = ~0ull, reflectBytes = 0;
    uint32_t materialPitch = 0, materialWidth = 0, materialHeight = 0, materialFormat = 0;
    bool materialAfterCompute = false; // the material target is finished by a compute write
    float reflStrength = 1.0f, smoothMin = 0.5f, f0 = 0.04f, reflEffective = 1.0f;
    uint32_t reflChannel = 1, reflInvert = 0, debugReflect = 0, materialSlot = 2, reflEverywhere = 0;
    // surface cache (live_surfcache): colour seen on screen of each triangle of the RT scene, for
    // reflections hitting a hidden or off-screen object (urti.ini surf_cache=0: off)
    ComPtr<ID3D12RootSignature> surfRs;
    ComPtr<ID3D12PipelineState> surfPso;
    ComPtr<ID3D12Resource> surfCache;
    uint64_t surfTriangles = 0, surfFilled = 0; // triangles held; frames where the cache was filled
    bool surfCacheOn = true;
    // RT scene of this frame, for filling the cache at composition: region table (upload buffer
    // rotating over three frames), number of regions, frame where it was used
    ID3D12Resource* sceneTable = nullptr;
    uint32_t sceneCount = 0;
    ComPtr<ID3D12Resource> sceneGpu; // the table copied into video memory every frame (read as an SRV until the next)
    bool sceneGpuReadable = false;
    // light bounce (indirect lighting, urti.ini gi=, 0: off): colour of the surfaces hit by the occlusion
    // rays, taken from the surface cache; three planes R, G, B at render resolution: raw (live_shadow),
    // denoised (live_shadowtemporal), previous (history). Received according to the surfaces' own colour (G-buffer
    // albedo, copied every frame)
    float giStrength = 1.0f;
    // ReShade menu checkboxes and sliders (urti.fx), read on every present: they multiply the urti.ini settings
    bool menuRefl = true, menuAo = true, menuGi = true;
    float menuAoForce = 1.0f, menuAoRadius = 0.0f, menuGiForce = 1.0f; // (range 0: urti.ini's)
    float menuSky = 1.0f;                                               // (multiplie ao_sky=)
    bool menuGloss = true; // "reflections on close glossy floors" checkbox
    bool aoMotionFull = true;
    bool denoiseDual = true;
    bool aoIgn = true;
    bool aoSunRay = true; // ao_sunray=: sun visibility through an RT ray for occlusion (bit 28), instead of the game's shadow map // ao_ign=: low-discrepancy occlusion ray directions (bit 27 of live_shadow's flags) // denoise_dual=: dual history (fast + slow) in the occlusion denoiser // ao_motion=: moving pixels computed every frame (0: quarter rate as before)
    float glossMin = 0.0f, glossAmount = 1.0f;                          // glossy reflections (gloss_min=, gloss=): minimum smoothness (0: off), strength
    float skyAmount = 0.7f, skyRange = 16.0f;                           // long-range sky visibility (ao_sky=, ao_sky_range=)
    bool personMask = true;     // urti.ini person_mask=: alpha of the G-buffer normal = characters (RDR2); 0: no marker
    ComPtr<ID3D12Resource> giRaw, giBuf, giPrev;
    bool giValid = false;       // giBuf written on the last frame (otherwise no history, no composition)
    uint64_t giStamp = ~0ull;
    ComPtr<ID3D12Resource> albedoBuf;
    uint64_t albedoBytes = 0, albedoFrame = ~0ull;
    uint32_t albedoPitch = 0, albedoWidth = 0, albedoHeight = 0, albedoFormat = 0;
    // G-buffer normal (target 1); its alpha marks characters (RDR2), who do not receive the bounce: they
    // move, their history is rejected every frame and the bounce, noisier than the occlusion, shook on them
    ComPtr<ID3D12Resource> normalBuf;
    uint64_t normalBytes = 0, normalFrame = ~0ull;
    uint32_t normalPitch = 0, normalWidth = 0, normalHeight = 0, normalFormat = 0;
    uint64_t sceneStamp = ~0ull;
    uint64_t maskFrame = ~0ull;    // frame where the shadow mask was computed (otherwise it no longer follows the camera)
    int lastMode = -1;
    float lastStrength = -1;
    bool lastSplit = false, lastShowMask = false, lastTechnique = true, iniMaskSet = false;
    std::atomic<bool> toggleSplit{ false }; // F5: split screen (left game alone, right with URTI)
    float sunSign = 1; // sun orientation relative to the maps' depth (the game's convention)
    uint32_t missedCalibrations = 0;
    ComPtr<ID3D12RootSignature> fitRs, voteRs;
    ComPtr<ID3D12PipelineState> fitPso, votePso;
    ComPtr<ID3D12Resource> pairsUpload[3], pairDirs, sunBuf, sunUpload, sunReadback[3];
    ComPtr<ID3D12Resource> regionUpload[3]; // table of the screen's regions for the GPU (per frame)
    // fixed places (g_livePersist): table of all held places, vertex counts kept from one frame to
    // the next (8 bytes per place), pairs (frame counter, count cell) of the frame's draws
    ComPtr<ID3D12Resource> placeUpload[3], srcUpload[3], fitUpload[3];
    std::unordered_map<uint32_t, uint32_t> idMesh; // identity of a copy -> its mesh (budget per mesh)
    ComPtr<ID3D12RootSignature> countsRs;
    ComPtr<ID3D12PipelineState> countsPso;
    uint32_t blasConverted = 0; // vertices of the last full build (update: same count)
    // places redistributed at the end of the previous frame, or many new ones in this one: the triangles
    // are no longer where the structure expects them, an update would make it very slow (v58: 4.5 fps);
    // full rebuild
    bool placesMoved = false, placesResetPending = false;
    // update or rebuild, on measured cost (GPU timers read three frames later): an update
    // costs less than a rebuild, but slows rays down; as soon as their slowdown
    // accumulated since the last rebuild exceeds what it saves, the structure is rebuilt
    uint32_t buildCycle = 0, slotCycle[3] = {};
    bool slotRefit[3] = {}, slotBuilt[3] = {};
    double rebuildMs = 2.0, refitMs = 0.5, traceBase = -1.0, refitExtra = 0.0;
    uint64_t statCycles = 0;
    uint64_t statPlaces = 0, statPlaceHeld = 0, statPlaceHigh = 0, statPlaceNew = 0, statPlaceFrames = 0, statPlaceResets = 0;
    ComPtr<ID3D12Resource> atlasBuf; // copy of the game's shadow map, to recompute its shadow
    // applied to the presented image (every present, generated frames included)
    ComPtr<ID3D12RootSignature> compositeRs;
    ComPtr<ID3D12PipelineState> compositePso;
    ComPtr<ID3D12Resource> imageIn, imageOut;
    uint64_t imageBytes = 0;
    // summary per 300 presents: without game draws, ReShade effects, shadows applied
    uint32_t statPresents = 0, statNoDraw = 0, statEffects = 0, statComposited = 0;
    uint32_t statNoDepth = 0, statMaskOld = 0, statGapMax = 0; // computations without depth, presents with a mask too old, longest gap between two computations
    uint32_t statSkipOverlay = 0, statSkipTechnique = 0; // presents without effect: ReShade menu open, effect off in ReShade
    // retired buffers, destroyed much later; present-queue fence, wait when a ring buffer (frame % 3) is
    // reused while the GPU has not read it yet
    struct RetiredBuffer
    {
        uint64_t frame = 0;
        ComPtr<ID3D12Resource> r;
    };
    std::vector<RetiredBuffer> retiredLong;
    ComPtr<ID3D12Fence> slotFence;
    HANDLE slotEvent = nullptr;
    uint64_t slotFenceValue = 0, slotDone[3] = {};
    uint32_t statSlotWaits = 0;
    // sub-pixel jitter of the last frame (shear of camera space, game camera)
    float camShear[2] = {};
    ComPtr<ID3D12Resource> jitUpload[3];
    // RT scene stability: frame where each mesh left it, round of each huge mesh
    std::unordered_map<uint32_t, uint64_t> meshLeft;
    std::unordered_map<uint32_t, uint32_t> meshPhase;
    // one game frame at a time: depth copies made inside the game's lists (video memory, common state: they move from one
    // queue to the other without a barrier), last view, computation of this present
    static constexpr uint32_t kDepthSnaps = 12;
    bool frameSync = false, syncNow = false, presentComputed = false;
    ComPtr<ID3D12Resource> depthSnap[kDepthSnaps];
    uint64_t depthSnapBytes = 0, snapSerialSeen = 0, snapSerialUsed = 0, snapSeenFrame = 0;
    uint32_t depthSnapPitch = 0, depthSnapWidth = 0, depthSnapHeight = 0;
    int32_t snapIndexNow = -1;
    // copies of the material image (same principle), the one of this computation
    static constexpr uint32_t kMaterialSnaps = 4;
    ComPtr<ID3D12Resource> materialSnap[kMaterialSnaps];
    // G-buffer normals (gnormal_slot=, gnormal=): ring of copies like the material (with frame_sync), otherwise one copy; comparison (log)
    ComPtr<ID3D12Resource> gnSnap[kMaterialSnaps], gnBuf, gnStats, gnStatsRb;
    uint64_t gnBytes = 0, gnFrame = ~0ull;
    uint32_t gnPitch = 0, gnWidth = 0, gnHeight = 0, gnFormat = 0;
    int gnSlot = -1;
    uint32_t gnMode = 0, gnSigns = 0;
    bool gnUse = false, gnCompare = true, gnReflect = true;
    ID3D12Resource* gnNowFrame = nullptr; // copy of the current frame's normals (read by the shadows, then the reflections)
    ComPtr<ID3D12RootSignature> rotfitRs;
    ComPtr<ID3D12PipelineState> rotfitPso;
    uint32_t rotLogFrames = 0;
    double rotSum[4] = {};
    int32_t materialIndexNow = -1;
    uint32_t statMaterialWait = 0, statMaterialLate = 0, statFramesSkipped = 0;
    // world vertical axis (from the camera), and the same axis in this frame's camera space: the water's normal
    double camUpScore[3] = {};
    float worldUp[3] = {};
    bool worldUpKnown = false, reflectUp = true;
    uint32_t statSnapUsed = 0, statSnapFallback = 0;
    // urti.ini frame_trace=1: one line per present, and a row of depth and mask per computation
    bool frameTrace = false;
    ComPtr<ID3D12Resource> traceRb[3];
    uint64_t traceFrame[3] = { ~0ull, ~0ull, ~0ull };
    uint32_t traceWidth[3] = {};
    // hot reload: settings (urti.ini) and shaders (urti_shaders\*.cso), without restarting the game
    FILETIME iniTime = {}, shaderTime[7] = {};
    std::vector<ComPtr<ID3D12PipelineState>> retiredPso[3];
    uint64_t atlasBytes = 0;
    uint32_t atlasPitch = 0;
    bool atlasD24 = false, atlas16 = false; // the game's shadow map in D24, in 16 bits (RDR2: R16, two bytes per texel)
    uint64_t framesSinceSun = 0, sunFrames = 0, sunAccepted = 0, pairSum = 0;
    uint32_t rotationValid = 0;
    double rotationSum = 0;
} live;
std::mutex g_liveRetireMutex;
constexpr uint32_t kLiveMaxPairs = 256;
// sun, summary, cascades (48 + 64 k), pending sun (560), camera rotation (576), cascades of
// the previous frame (640)
constexpr uint32_t kLiveMaxMotionPairs = 32768; // vertex pairs for the camera motion (live_camfit)
constexpr uint32_t kLiveSunBytes = 2176; // + tracking of the game's sub-pixel jitter (1168 to 1199), stretch of the projection (1200 to 1223),
                                                             // sun share in the image (1232: pairs, 1248: histograms, 1888 to 2047: check, live_sunlight)

// ------------------------------------------------------------------------------------------------
// stored calibration: depth law, shadow-map convention, description of that map
// ------------------------------------------------------------------------------------------------

void LiveSaveCalibration()
{
    // invalid depth law: nothing is written (read back at launch, a NaN law made every screen point
    // invalid: no more occlusion or reflection in a game we tested, until the frame's measurement)
    if (!std::isfinite(live.A) || !std::isfinite(live.B) || live.B == 0)
        return;
    if (FILE* f = std::fopen((g_dir + "\\urti_calib.ini").c_str(), "w"))
    {
        std::fprintf(f, "; URTI calibration for this game (read back at launch; F6 redoes it)\n");
        std::fprintf(f, "depth_a=%.9g\ndepth_b=%.9g\nsun_sign=%d\nsun_map=%ux%u:%u\nmotion=%u\nmotion_target=%u\nmotion_source=%s\n", live.A, live.B,
                     live.sunSign > 0 ? 1 : -1, live.sunMapWidth, live.sunMapHeight, live.sunMapFormat, live.mvSynthetic ? 255u : live.mvConvention,
                     live.mvCandidate, live.mvSynthetic ? "camera" : "jeu");
        if (live.fovMeasured && live.p11 > 0 && !live.camLive)
            std::fprintf(f, "fov_v=%.2f\n", 2.0f * std::atan(1.0f / live.p11) * 57.2957795f); // measured vertical field of view (degrees)
        std::fclose(f);
    }
}

// assumed projection (arbitrary field of view, like Calibrate) for the width / height ratio
void LiveSetProjection(const D3D12_RESOURCE_DESC& depthDesc)
{
    const bool stored = live.fovStored >= 15.0f && live.fovStored <= 80.0f;
    const float fov = (stored ? live.fovStored : 50.0f) * 3.14159265f / 180.0f;
    const float aspect = depthDesc.Height ? float(depthDesc.Width) / float(depthDesc.Height) : 16.0f / 9.0f;
    live.p11 = 1.0f / std::tan(fov * 0.5f), live.p00 = live.p11 / aspect;
    if (stored)
        live.fovMeasured = true;
}

// on the first present: calibration read back; the shadow map will be recognised by its description
void LiveLoadCalibration()
{
    if (live.storeLoaded)
        return;
    live.storeLoaded = true;
    FILE* f = std::fopen((g_dir + "\\urti_calib.ini").c_str(), "r");
    if (!f)
        return;
    double a = 0, b = 0;
    int sign = 0;
    unsigned w = 0, h = 0, fmt = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f))
    {
        std::sscanf(line, "depth_a=%lf", &a), std::sscanf(line, "depth_b=%lf", &b), std::sscanf(line, "sun_sign=%d", &sign);
        std::sscanf(line, "sun_map=%ux%u:%u", &w, &h, &fmt);
        unsigned motionConvention = 255;
        if (std::sscanf(line, "motion=%u", &motionConvention) == 1 && motionConvention < 16)
            live.mvConvention = motionConvention;
        std::sscanf(line, "motion_target=%u", &live.mvCandidate);
        std::sscanf(line, "fov_v=%f", &live.fovStored);
        if (std::strncmp(line, "motion_source=camera", 20) == 0 && live.motionSource != 1)
            live.mvSynthetic = true, live.mvConvention = 0;
    }
    std::fclose(f);
    if (b == 0 || !std::isfinite(a) || !std::isfinite(b) || !sign || !w)
        return;
    live.p00 = 0; // set at the first depth (width / height ratio)
    live.A = float(a), live.B = float(b), g.depthReversed = b > 0, live.sunSign = float(sign);
    live.sunMapWidth = w, live.sunMapHeight = h, live.sunMapFormat = fmt;
    live.depthReady = live.calibrated = live.fromStore = true;
    live.framesSinceSun = 1000; // no shadows before the first sun measurement (a few frames)
    g_liveSunMapDesc[0] = w, g_liveSunMapDesc[1] = h, g_liveSunMapDesc[2] = fmt;
    Log("URTI live: stored calibration read back (depth = %.6f + %.6f / w, shadow map %ux%u format %u): effects from the first frame",
        a, b, w, h, fmt);
}

// ------------------------------------------------------------------------------------------------
// calibration: sun direction (shadow maps) and depth law, on the CPU
// ------------------------------------------------------------------------------------------------

// least squares S ≈ X M (X: n × 4, S: n × 3) through the normal equations; false if ill-conditioned
bool SolveAffine(const std::vector<double>& X, const std::vector<double>& S, size_t n, double M[4][3])
{
    double N[4][4] = {}, R[4][3] = {};
    for (size_t i = 0; i < n; i++)
        for (int a = 0; a < 4; a++)
        {
            for (int b = 0; b < 4; b++)
                N[a][b] += X[i * 4 + a] * X[i * 4 + b];
            for (int c = 0; c < 3; c++)
                R[a][c] += X[i * 4 + a] * S[i * 3 + c];
        }
    for (int col = 0; col < 4; col++) // Gauss with partial pivoting
    {
        int p = col;
        for (int r = col + 1; r < 4; r++)
            if (std::fabs(N[r][col]) > std::fabs(N[p][col]))
                p = r;
        if (std::fabs(N[p][col]) < 1e-12)
            return false;
        for (int k = 0; k < 4; k++)
            std::swap(N[col][k], N[p][k]);
        for (int k = 0; k < 3; k++)
            std::swap(R[col][k], R[p][k]);
        for (int r = 0; r < 4; r++)
        {
            if (r == col)
                continue;
            const double f = N[r][col] / N[col][col];
            for (int k = 0; k < 4; k++)
                N[r][k] -= f * N[col][k];
            for (int k = 0; k < 3; k++)
                R[r][k] -= f * R[col][k];
        }
    }
    for (int a = 0; a < 4; a++)
        for (int c = 0; c < 3; c++)
            M[a][c] = R[a][c] / N[a][a];
    return true;
}

// From a captured file (vertices of every stream, bounds of the draws): sun and depth.
void Calibrate(const uint8_t* so, const uint32_t* counters, const uint64_t* marks)
{
    const int mainSlot = g.captureSlot;
    if (mainSlot < 0)
    {
        live.missedCalibrations++; // (no screen: the next search later)
        return;
    }
    const float fov = 50.0f * 3.14159265f / 180.0f; // arbitrary value: shadows do not depend on it
    const float aspect = g.depthDesc.Height ? float(g.depthDesc.Width) / float(g.depthDesc.Height) : 16.0f / 9.0f;
    const float p11 = 1.0f / std::tan(fov * 0.5f), p00 = p11 / aspect;
    auto vertex = [&](int slot, uint64_t byteOffset) { return reinterpret_cast<const float*>(so + slot * kSlotBytes + byteOffset); };

    // depth law: clip z = A w + B, fitted on the screen's vertices
    double sw = 0, sz = 0, sww = 0, swz = 0, n = 0;
    const uint64_t mainBytes = std::min<uint64_t>(counters[mainSlot * 2], kSlotBytes);
    const uint64_t sampleStep = 16 * std::max<uint64_t>(1, mainBytes / 16 / 20000);
    for (uint64_t o = 0; o + 16 <= mainBytes; o += sampleStep)
    {
        const float* c = vertex(mainSlot, o);
        if (c[3] > 1e-3f)
            sw += c[3], sz += c[2], sww += double(c[3]) * c[3], swz += double(c[3]) * c[2], n += 1;
    }
    if (n < 30)
    {
        Log("URTI live: calibration impossible (%u vertices on the screen depth)", unsigned(n));
        return;
    }
    const double A = (n * swz - sw * sz) / (n * sww - sw * sw), B = (sz - A * sw) / n;
    if (!std::isfinite(A) || !std::isfinite(B) || B == 0)
    {
        Log("URTI live: calibration impossible (degenerate depth law)");
        return;
    }
    if (!live.depthReady)
        Log("URTI live: depth law = %.6f + %.6f / w (reflections possible even without a sun)", A, B);
    live.A = float(A), live.B = float(B);
    if (!live.fovMeasured || live.p11 <= 0) // (measured field of view: kept; the calibration's is arbitrary, 50°)
        live.p00 = p00, live.p11 = p11;
    g.depthReversed = B > 0;
    live.depthReady = true;

    // sun: for each shadow map, one direction per pair of draws, then a vote
    struct Dir { double d[3]; double w; };
    double bestSun[3] = { 0, 1, 0 }, bestScore = 0;
    uint32_t bestSlot = 0;
    for (uint32_t s = 0; s < kSlots; s++)
    {
        if (int(s) == mainSlot || !g.slotDepth[s] || g.screenLike[g.slotDepth[s]] == 1)
            continue;
        std::vector<Dir> dirs;
        std::unordered_map<uint64_t, std::vector<uint32_t>> theirs;
        auto key = [](const DrawRecord& r, uint64_t bytes) {
            uint64_t k = r.kind * 1000003ull + bytes;
            for (uint32_t p : r.params)
                k = k * 1315423911ull + p;
            return k;
        };
        for (uint32_t d = 0; d < g.captureDrawCount; d++)
            if (g.draws[d].slot == s && marks[d * 2 + 1] > marks[d * 2])
                theirs[key(g.draws[d], marks[d * 2 + 1] - marks[d * 2])].push_back(d);
        std::vector<double> X, S;
        std::unordered_map<uint64_t, uint32_t> rank; // rank of the draw among those with the same signature
        for (uint32_t d = 0; d < g.captureDrawCount && dirs.size() < 4000; d++)
        {
            if (g.draws[d].slot != uint32_t(mainSlot) || marks[d * 2 + 1] <= marks[d * 2])
                continue;
            const uint64_t bytes = marks[d * 2 + 1] - marks[d * 2];
            const uint64_t k0 = key(g.draws[d], bytes);
            const uint32_t myRank = rank[k0]++;
            auto it = theirs.find(k0);
            if (it == theirs.end())
                continue;
            const uint64_t verts = bytes / 16, step = std::max<uint64_t>(1, verts / 256);
            for (size_t m = 0; m < it->second.size() && m < 64; m++)
            {
                const uint32_t e = it->second[m];
                X.clear(), S.clear();
                for (uint64_t v = 0; v < verts; v += step)
                {
                    const float* c = vertex(mainSlot, marks[d * 2] + v * 16);
                    const float* l = vertex(int(s), marks[e * 2] + v * 16);
                    if (c[3] <= 1e-3f || std::fabs(l[3]) < 1e-6f)
                        continue;
                    X.insert(X.end(), { c[0] / p00, c[1] / p11, double(c[3]), 1.0 });
                    S.insert(S.end(), { double(l[0]) / l[3], double(l[1]) / l[3], double(l[2]) / l[3] });
                }
                const size_t k = X.size() / 4;
                double M[4][3];
                if (k < 24 || !SolveAffine(X, S, k, M))
                    continue;
                // faithful fit? (otherwise another object, or animated)
                std::vector<double> res(k);
                double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
                for (size_t i = 0; i < k; i++)
                {
                    double r2 = 0;
                    for (int c = 0; c < 3; c++)
                    {
                        double pred = M[3][c];
                        for (int a = 0; a < 3; a++)
                            pred += X[i * 4 + a] * M[a][c];
                        r2 += (pred - S[i * 3 + c]) * (pred - S[i * 3 + c]);
                        lo[c] = std::min(lo[c], S[i * 3 + c]), hi[c] = std::max(hi[c], S[i * 3 + c]);
                    }
                    res[i] = std::sqrt(r2);
                }
                std::nth_element(res.begin(), res.begin() + k / 2, res.end());
                const double scale = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] }) + 1e-12;
                if (res[k / 2] > 1e-3 * scale)
                    continue;
                // d such that A d = (0, 0, 1), A = M[0..2]ᵀ: the direction of the map's depth
                const double a[3][3] = { { M[0][0], M[1][0], M[2][0] }, { M[0][1], M[1][1], M[2][1] }, { M[0][2], M[1][2], M[2][2] } };
                const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                                   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
                if (std::fabs(det) < 1e-18)
                    continue;
                // third column of the inverse: cofactors of the third row
                const double dx = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
                const double dy = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
                const double dz = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                // same rank in both passes: very probably the same object (repeated meshes)
                dirs.push_back({ { -dx / len, -dy / len, -dz / len }, double(k) * (m == myRank ? 8.0 : 1.0) });
            }
        }
        // direction (up to sign) on which the most vertices agree, within 2°
        const double cosTol = std::cos(2.0 * 3.14159265 / 180.0);
        double slotBest = 0;
        size_t bestIndex = 0;
        for (size_t i = 0; i < dirs.size(); i++)
        {
            double vote = 0;
            for (const Dir& o : dirs)
                if (std::fabs(dirs[i].d[0] * o.d[0] + dirs[i].d[1] * o.d[1] + dirs[i].d[2] * o.d[2]) > cosTol)
                    vote += o.w;
            if (vote > slotBest)
                slotBest = vote, bestIndex = i;
        }
        if (dirs.empty() || slotBest <= bestScore)
            continue;
        double sum[3] = {};
        for (const Dir& o : dirs)
        {
            const double c = dirs[bestIndex].d[0] * o.d[0] + dirs[bestIndex].d[1] * o.d[1] + dirs[bestIndex].d[2] * o.d[2];
            if (std::fabs(c) > cosTol)
                for (int q = 0; q < 3; q++)
                    sum[q] += (c > 0 ? 1 : -1) * o.d[q] * o.w;
        }
        const double len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        // vertical view (rain, occlusion): not the sun; view along the camera's axis (reduced screen depth for culling hidden
        // objects, 1024 × 1024 in Red Dead): not the sun either. A Red Dead calibration had taken it (sun (0.002 0.059 0.998)) and had
        // stored it: no sun tracked since
        if (len <= 0 || std::fabs(sum[1] / len) > 0.995 || std::fabs(sum[2] / len) > 0.97)
            continue;
        bestScore = slotBest;
        bestSlot = s;
        for (int q = 0; q < 3; q++)
            bestSun[q] = sum[q] / len;
    }
    if (bestScore <= 0)
    {
        // overcast, night, indoors: no sun, no shadows (after three attempts in a row)
        if (++live.missedCalibrations >= 3 && live.calibrated)
        {
            live.calibrated = false;
            Log("URTI live: no sun shadow map any more, RT shadows suspended");
        }
        else
            Log("URTI live: calibration without a sun shadow map");
        return;
    }
    live.missedCalibrations = 0;
    g_liveSunDepth = g.slotDepth[bestSlot];
    live.sunDirty = true;
    live.framesSinceSun = 0;
        // per-pair directions have a defined orientation (decreasing depth of the map); the sun is
        // above the horizon: this choice sets the game's convention for frame-by-frame tracking
    live.sunSign = bestSun[1] < 0 ? -1.0f : 1.0f;
    if (bestSun[1] < 0)
        for (double& v : bestSun)
            v = -v;
    for (int q = 0; q < 3; q++)
        live.sun[q] = float(bestSun[q]);
    if (!std::isfinite(A) || !std::isfinite(B) || B == 0)
        return; // degenerate law: nothing is adopted
    live.A = float(A), live.B = float(B);
    if (!live.fovMeasured || live.p11 <= 0) // (measured field of view: kept; the calibration's is arbitrary, 50°)
        live.p00 = p00, live.p11 = p11;
    g.depthReversed = B > 0; // reversed depth: d = B / w, the sky (infinite w) at 0
    live.calibrated = true;
    live.verified = true;
    Log("URTI live: sun (%.3f, %.3f, %.3f), depth = %.6f + %.6f / w", bestSun[0], bestSun[1], bestSun[2], A, B);
    if (ID3D12Resource* map = reinterpret_cast<ID3D12Resource*>(g_liveSunDepth))
    {
        const D3D12_RESOURCE_DESC d = map->GetDesc();
        const bool changed = live.fromStore && (std::fabs(live.A - float(A)) > 1e-4f * std::max(1.0f, std::fabs(float(A))) ||
                                                std::fabs(live.B - float(B)) > 1e-3f * std::fabs(float(B)) || live.sunMapWidth != uint32_t(d.Width) ||
                                                live.sunMapHeight != d.Height || live.sunMapFormat != uint32_t(d.Format));
        live.sunMapWidth = uint32_t(d.Width), live.sunMapHeight = d.Height, live.sunMapFormat = uint32_t(d.Format);
        LiveSaveCalibration();
        if (changed)
            Log("URTI live: the stored calibration no longer matched, it is replaced");
    }
}

// ------------------------------------------------------------------------------------------------
// GPU: acceleration structure and shadows, before ReShade's effects
// ------------------------------------------------------------------------------------------------

bool LiveCreatePipelines(ID3D12Device* device)
{
    auto make = [&](const void* code, size_t size, int srvCount, ComPtr<ID3D12RootSignature>& rs, ComPtr<ID3D12PipelineState>& pso, uint32_t constants,
                    int uavCount = 1) {
        D3D12_ROOT_PARAMETER p[24] = {}; // SRV (t0…), UAV (u0…), constantes
        for (int i = 0; i < srvCount; i++)
        {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[i].Descriptor.ShaderRegister = i;
        }
        for (int i = 0; i < uavCount; i++)
        {
            p[srvCount + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[srvCount + i].Descriptor.ShaderRegister = i;
        }
        p[srvCount + uavCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[srvCount + uavCount].Constants.Num32BitValues = constants;
        D3D12_ROOT_SIGNATURE_DESC d = { UINT(srvCount + uavCount + 1), p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(g_createRootSignature(device, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = rs.Get();
        cd.CS = { code, size };
        return SUCCEEDED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso)));
    };
    return make(g_live_convert, sizeof(g_live_convert), 4, live.convertRs, live.convertPso, 24, 2) &&
           make(g_live_geoconvert, sizeof(g_live_geoconvert), 3, live.geoconvRs, live.geoconvPso, 4, 2) &&
           make(g_live_geoinst, sizeof(g_live_geoinst), 0, live.geoinstRs, live.geoinstPso, 4, 2) &&
           make(g_live_refpairs, sizeof(g_live_refpairs), 3, live.refpairsRs, live.refpairsPso, 8, 3) &&
           make(g_live_objplanes, sizeof(g_live_objplanes), 3, live.objplanesRs, live.objplanesPso, 4, 1) &&
           make(g_live_camfit, sizeof(g_live_camfit), 0, live.camfitRs, live.camfitPso, 4, 2) &&
           make(g_live_mvsynth, sizeof(g_live_mvsynth), 3, live.mvsynthRs, live.mvsynthPso, 8) &&
           make(g_live_planes, sizeof(g_live_planes), 2, live.planesRs, live.planesPso, 4, 2) &&
           make(g_live_shadow, sizeof(g_live_shadow), 11, live.shadowRs, live.shadowPso, 34, 4) &&
           make(g_live_mvcal, sizeof(g_live_mvcal), 4, live.mvcalRs, live.mvcalPso, 8) &&
           make(g_live_shadowtemporal, sizeof(g_live_shadowtemporal), 7, live.denoiseRs, live.denoisePso, 15, 2) &&
           make(g_live_sunfit, sizeof(g_live_sunfit), 3, live.fitRs, live.fitPso, 4) &&
           make(g_live_sunvote, sizeof(g_live_sunvote), 1, live.voteRs, live.votePso, 8) &&
           make(g_live_composite, sizeof(g_live_composite), 15, live.compositeRs, live.compositePso, 29, 2) &&
           make(g_live_delay, sizeof(g_live_delay), 4, live.delayRs, live.delayPso, 8, 1) &&
           make(g_live_waterbase, sizeof(g_live_waterbase), 3, live.waterBaseRs, live.waterBasePso, 12, 1) &&
           make(g_live_rotfit, sizeof(g_live_rotfit), 0, live.rotfitRs, live.rotfitPso, 4, 2) &&
           make(g_live_sunlight, sizeof(g_live_sunlight), 3, live.sunlightRs, live.sunlightPso, 20) &&
           make(g_live_counts, sizeof(g_live_counts), 2, live.countsRs, live.countsPso, 4) &&
           make(g_live_uicompare, sizeof(g_live_uicompare), 6, live.uiRs, live.uiPso, 8) &&
           make(g_live_uiselect, sizeof(g_live_uiselect), 5, live.uiSelRs, live.uiSelPso, 8, 2) &&
           make(g_live_hudless, sizeof(g_live_hudless), 4, live.hudRs, live.hudPso, 8, 3) &&
           make(g_live_hudgain, sizeof(g_live_hudgain), 0, live.hudGainRs, live.hudGainPso, 8, 1) &&
           make(g_live_film, sizeof(g_live_film), 1, live.filmRs, live.filmPso, 8, 1) &&
           make(g_live_reflresolve, sizeof(g_live_reflresolve), 10, live.resolveRs, live.resolvePso, 22, 2) &&
           make(g_live_reflect, sizeof(g_live_reflect), 15, live.reflectRs, live.reflectPso, 26, 4) &&
           make(g_live_radcache, sizeof(g_live_radcache), 6, live.radRs, live.radPso, 17, 3) &&
           make(g_live_surfcache, sizeof(g_live_surfcache), 5, live.surfRs, live.surfPso, 18, 2) &&
           make(g_live_regionstats, sizeof(g_live_regionstats), 3, live.statsRs, live.statsPso, 4) &&
           make(g_live_depthfit, sizeof(g_live_depthfit), 3, live.depthfitRs, live.depthfitPso, 4);
}

ComPtr<ID3D12Resource> UavBuffer(ID3D12Device* device, uint64_t size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

void Retire(ComPtr<ID3D12Resource>& r)
{
    // kept 120 presents, not three: with frame generation, three presents make a frame and a half, and during a
    // load the GPU lags further behind than that. Destroying a buffer still in use loses the GPU
    if (r)
    {
        std::lock_guard<std::mutex> lock(g_liveRetireMutex);
        live.retiredLong.push_back({ live.frame, r });
    }
    r.Reset();
}

// geometry cache: sizes per frame and of the top-level structure
constexpr uint32_t kLiveGeoMaxInstances = 16384, kLiveGeoMaxBuilds = 1024, kLiveGeoFrameVerts = 2000000, kLiveGeoMeshVerts = 150000, kLiveSentinels = 96;
constexpr uint64_t kLiveGeoScratchBytes = 192ull * 1048576;

// acceleration structure large enough for `vertices` vertices
bool LiveEnsureCapacity(ID3D12Device5* device, uint64_t vertices)
{
    if (vertices <= live.capacity && live.blas)
        return true;
    const uint64_t cap = std::min<uint64_t>(kSlotBytes / 16, (vertices * 3 / 2 + 65535) / 3 * 3);
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexBuffer.StrideInBytes = 12;
    geom.Triangles.VertexCount = UINT(cap);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.NumDescs = 1;
    in.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    info.ScratchDataSizeInBytes = std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes);
    live.blasAge = ~0u; // new structure: full build
    Retire(live.view), Retire(live.blas), Retire(live.blasScratch);
    live.view = UavBuffer(device, cap * 12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Retire(live.viewPrev);
    live.viewPrev = UavBuffer(device, cap * 12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    live.prevValid = false;
    live.blas = UavBuffer(device, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    live.blasScratch = UavBuffer(device, info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.capacity = cap;
    if (!live.tlas)
    {
        D3D12_RAYTRACING_INSTANCE_DESC inst = {};
        inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
        inst.InstanceMask = 0xFF;
        live.instances = MakeBuffer(device, 2 * sizeof(inst), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin = {};
        tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tin.NumDescs = 2 + kLiveGeoMaxInstances; // opaque scene (mask 1), flat transparent surfaces (mask 2), geometry cache (mask 1)
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ti = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &ti);
        live.tlas = UavBuffer(device, ti.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        live.tlasScratch = UavBuffer(device, ti.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Log("URTI live: acceleration structure for %llu vertices (%.1f MB)", (unsigned long long)cap, info.ResultDataMaxSizeInBytes / 1048576.0);
    return live.view && live.blas && live.blasScratch && live.tlas && live.instances;
}

// images (depth, shadows) at the size of the game's depth
// rendered area: where the game draws (viewports of its screen draws), otherwise the whole depth.
// With DLSS or dynamic resolution, some engines draw into a corner of full-size targets:
// read whole, shadows and reflections fell shifted and stretched (×1.5 in a game we tested)
void LiveRenderArea(const D3D12_RESOURCE_DESC& depthDesc, uint32_t& w, uint32_t& h)
{
    w = uint32_t(depthDesc.Width), h = depthDesc.Height;
    const uint32_t vw = uint32_t(live.viewMax[0] + 0.5f), vh = uint32_t(live.viewMax[1] + 0.5f);
    if (live.renderAreaOn && vw >= 64 && vh >= 64 && vw <= w && vh <= h && (vw < w || vh < h))
        w = vw, h = vh;
}

bool LiveEnsureImages(effect_runtime* runtime, ID3D12Device* device, const D3D12_RESOURCE_DESC& depthDesc)
{
    uint32_t renderW = 0, renderH = 0;
    LiveRenderArea(depthDesc, renderW, renderH);
    if (live.mask && live.allocWidth == depthDesc.Width && live.allocHeight == depthDesc.Height && live.depthFormat == depthDesc.Format)
    {
        if (renderW != live.width || renderH != live.height)
            Log("URTI live: rendered area %ux%u inside the depth %llux%u", renderW, renderH, (unsigned long long)depthDesc.Width, depthDesc.Height);
        live.width = renderW, live.height = renderH; // the (full-size) images are used as they are
        return true;
    }
    Retire(live.depthBuf), Retire(live.shadowBuf), Retire(live.mask), Retire(live.prevShadowBuf);
    if (live.maskSrv.handle)
        runtime->get_device()->destroy_resource_view(live.maskSrv), live.maskSrv = {};
    live.allocWidth = uint32_t(depthDesc.Width), live.allocHeight = depthDesc.Height, live.depthFormat = depthDesc.Format;
    // images allocated at full size (dynamic resolution changes the rendered area without reallocating everything)
    live.width = live.allocWidth, live.height = live.allocHeight;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&depthDesc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    live.depthPitch = fp.Footprint.RowPitch;
    live.depthBuf = UavBuffer(device, total, D3D12_RESOURCE_STATE_COPY_DEST);
    live.shadowPitch = (live.width * 4 + 255) / 256 * 256;
    live.shadowBuf = UavBuffer(device, uint64_t(live.shadowPitch) * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.prevShadowBuf = UavBuffer(device, uint64_t(live.shadowPitch) * live.height, D3D12_RESOURCE_STATE_COPY_DEST);
    Retire(live.shadowRawBuf);
    live.shadowRawBuf = UavBuffer(device, uint64_t(live.shadowPitch) * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (auto& b : live.denoiseLen) // denoiser history length (zero: nothing accumulated)
        Retire(b), b = UavBuffer(device, uint64_t(live.shadowPitch) * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Retire(live.giRaw), Retire(live.giBuf), Retire(live.giPrev);
    live.giRaw = UavBuffer(device, 3ull * live.shadowPitch * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.giBuf = UavBuffer(device, 3ull * live.shadowPitch * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.giPrev = UavBuffer(device, 3ull * live.shadowPitch * live.height, D3D12_RESOURCE_STATE_COPY_DEST);
    live.giValid = false;
    Retire(live.depthPrevBuf);
    live.depthPrevBuf = UavBuffer(device, total, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = live.width;
    td.Height = live.height;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&live.mask))))
        return false;
    if (!runtime->get_device()->create_resource_view(resource{ reinterpret_cast<uint64_t>(live.mask.Get()) }, resource_usage::shader_resource,
                                                     resource_view_desc(format::r32_float), &live.maskSrv))
        return false;
    runtime->update_texture_bindings("URTI_SHADOW", live.maskSrv, live.maskSrv);
    for (auto& r : live.counterReadback)
        if (!r)
            r = MakeBuffer(device, kSlots * 8ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    live.width = renderW, live.height = renderH;
    if (renderW != live.allocWidth || renderH != live.allocHeight)
        Log("URTI live: rendered area %ux%u inside the depth %ux%u", renderW, renderH, live.allocWidth, live.allocHeight);
    return live.depthBuf && live.shadowBuf;
}

// sun-tracking buffers (created once)
bool LiveEnsureSun(ID3D12Device* device)
{
    if (live.sunBuf)
        return true;
    for (auto& r : live.pairsUpload)
        r = MakeBuffer(device, kLiveMaxPairs * 48ull, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    for (auto& r : live.sunReadback)
        r = MakeBuffer(device, kLiveSunBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    live.pairDirs = UavBuffer(device, kLiveMaxPairs * 80ull, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.sunBuf = UavBuffer(device, kLiveSunBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.sunUpload = MakeBuffer(device, 48, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    return live.sunBuf && live.pairDirs && live.sunUpload && live.pairsUpload[2] && live.sunReadback[2];
}

FILETIME FileTime(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a))
        return {};
    return a.ftLastWriteTime;
}

bool Newer(const FILETIME& a, const FILETIME& b) { return CompareFileTime(&a, &b) > 0; }

#include "urti_nrd.inl" // NVIDIA denoisers (if the add-on is built with NRD)

// every 30 frames: urti.ini modified -> settings read again; shader recompiled in urti_shaders -> pipeline
// recreated (the old one is released three frames later)
void LiveHotReload(ID3D12Device* device)
{
    if (live.frame % 30 != 0)
        return;
    const std::string ini = g_dir + "\\urti.ini";
    if (const FILETIME t = FileTime(ini); Newer(t, live.iniTime))
    {
        const bool first = live.iniTime.dwLowDateTime == 0 && live.iniTime.dwHighDateTime == 0;
        live.iniTime = t;
        if (FILE* f = std::fopen(ini.c_str(), "r"))
        {
            // default values at every reread: a line removed from the file gives its setting back its original value
            float combine = -1.0f, mask = 0.0f, debug = 0.0f, gameBias = 0.0015f, rayBias = 0.003f, sunRadius = 0.5f;
            float refl = 1.0f, channel = 1, invert = 0, smoothMin = 0.5f, f0 = 0.04f, debugReflect = 0, slot = 2, everywhere = 0, rays = 2, debugView = -1;
            float reflAlphaMin = 0.1f, reflAlphaMax = 0.5f, reflGamma = 1.0f, reflDepthTol = 0.1f, anchorAlpha = 0.0f, anchorGamma = 2.5f, alphaScreen = 1, blasRefit = 0, persist = 1, jitter = 2, fovAuto = 1, sunMeasure = 1, gameDilate = 1, gameMin = 1, ao = 1.0f, aoRadius = 1.5f, sunShadows = 0, aoQuarter = 1, buildMs = 2.5f, surfCache = 1, aoMs = 2.5f, gi = 1.0f, personMask = 1, motionSource = 0, renderArea = 1, water = 0, aoRoughIni = 0, nrdOn = 0, nrdDisocclusion = 0.01f, nrdFrames = 5;
            float vertexBudget = 6000000, probeBudget = 250000, slotLimit = 0, uiPhoto = 1, geoIni = 1, geoMb = 1000, motionLogIni = 0, cameraIni = 1, queueSyncIni = 1, traceDraw = 0, reflectDiag = 0, depthLawFixedIni = -1; // budget: 6 M out of a slot's 8 M
            float zone[4] = {};
            bool traceIni = false;
            float frameSyncIni = 1, reflectUpIni = 1, hudlessIni = 1, fgShiftIni = 0.5f, geoNewIni = 300000, geoRefreshIni = 100000, aoBoostIni = 0, slowRaysIni = 0, reflMsIni = 3.5f, reflectAllIni = 0, reflStatsIni = 0, waterBaseIni = 0, waterGainIni = 1.5f, radIni = 1, radVoxelIni = 0.005f, lagIni = 1, lagBiasIni = 0, reflFullIni = 1, denoiseLenIni = 1, reflSoftIni = 1.0f, reflSpatialIni = 0.5f, reflCoherentIni = 1, geoProbeIni = 2000000, sunEighthIni = 0, denoiseFarIni = 1, gnSlotIni = -1, gnModeIni = 0, gnSignsIni = 0, gnUseIni = 0, gnCompareIni = 1, gnReflectIni = 1, tessIni = 0, aoMotionIni = 1, dualIni = 1, ignIni = 1, sunRayIni = 1, fovForceIni = 0, skyIni = 0.7f, skyRangeIni = 16.0f, glossMinIni = 0.0f, glossIni = 1.0f;
            char line[256];
            while (std::fgets(line, sizeof(line), f))
            {
                std::string l = line;
                l.erase(l.find_last_not_of(" \r\n\t") + 1);
                auto value = [&](const char* key, float& out) {
                    if (l.rfind(key, 0) == 0)
                        out = float(std::atof(l.c_str() + std::strlen(key)));
                };
                value("combine=", combine), value("show_mask=", mask), value("debug_game_shadow=", debug);
                value("game_bias=", gameBias), value("ray_bias=", rayBias), value("sun_radius=", sunRadius);
                value("reflect=", refl), value("reflect_channel=", channel), value("reflect_invert=", invert), value("smooth_min=", smoothMin);
                value("reflect_f0=", f0), value("debug_reflect=", debugReflect), value("material_slot=", slot), value("reflect_everywhere=", everywhere);
                value("shadow_rays=", rays), value("debug_view=", debugView);
                value("refl_alpha_min=", reflAlphaMin), value("refl_alpha_max=", reflAlphaMax);
                value("refl_gamma=", reflGamma), value("refl_depth_tol=", reflDepthTol);
                value("anchor_alpha=", anchorAlpha), value("anchor_gamma=", anchorGamma); // anchor_alpha=0: anchoring off
                value("alpha_screen=", alphaScreen), value("blas_refit=", blasRefit); // blas_refit=0: always rebuild
                value("persist=", persist); // persist=0: regions assigned one after the other every frame (old rule)
                // geometry budget (vertices; 0: no budget), re-measurements per frame, smaller slot (test)
                value("vertex_budget=", vertexBudget), value("probe_budget=", probeBudget), value("slot_limit=", slotLimit), value("ui_photo=", uiPhoto),
                value("camera=", cameraIni), value("geo=", geoIni), value("geo_mb=", geoMb), value("motion_log=", motionLogIni), value("queue_sync=", queueSyncIni), value("trace_draw=", traceDraw), value("reflect_diag=", reflectDiag), value("depth_law_fixed=", depthLawFixedIni);
                value("nrd=", nrdOn), value("nrd_disocclusion=", nrdDisocclusion), value("nrd_frames=", nrdFrames), value("jitter=", jitter), value("fov_auto=", fovAuto);
                value("sun_measure=", sunMeasure), value("game_dilate=", gameDilate), value("game_min=", gameMin), value("ao=", ao), value("ao_radius=", aoRadius);
                value("person_mask=", personMask), value("motion_source=", motionSource), value("render_area=", renderArea), value("water=", water), value("ao_rough=", aoRoughIni);
                if (l.rfind("ui_zone=", 0) == 0)
                    std::sscanf(l.c_str() + 8, "%f,%f,%f,%f", &zone[0], &zone[1], &zone[2], &zone[3]);
                value("shadows=", sunShadows), value("ao_quarter=", aoQuarter), value("build_ms=", buildMs), value("surf_cache=", surfCache), value("ao_ms=", aoMs), value("gi=", gi);
                float jl = 0;
                value("jitter_log=", jl);
                float ft = 0;
                value("frame_trace=", ft);
                value("frame_sync=", frameSyncIni);
                value("reflect_up=", reflectUpIni); // 0: water normal from depth (old rule, to compare)
                value("hudless=", hudlessIni);      // 0: without the game's image before its UI (control)
                value("fg_shift=", fgShiftIni);     // generated frames: share of the motion (0: shadow and reflection frozen, old rule)
                value("geo_new=", geoNewIni), value("geo_refresh=", geoRefreshIni), value("ao_boost=", aoBoostIni), value("rayons_lents=", slowRaysIni), value("refl_ms=", reflMsIni), value("reflect_all=", reflectAllIni), value("reflect_stats=", reflStatsIni), value("water_base=", waterBaseIni), value("water_gain=", waterGainIni), value("radcache=", radIni), value("rad_voxel=", radVoxelIni), value("lag=", lagIni), value("lag_bias=", lagBiasIni), value("refl_full=", reflFullIni), value("denoise_len=", denoiseLenIni), value("refl_soft=", reflSoftIni), value("refl_spatial=", reflSpatialIni), value("refl_coherent=", reflCoherentIni), value("geo_probe=", geoProbeIni), value("sun_eighth=", sunEighthIni), value("denoise_far=", denoiseFarIni), value("gnormal_slot=", gnSlotIni), value("gnormal=", gnModeIni), value("gnormal_signs=", gnSignsIni), value("gnormal_use=", gnUseIni), value("gnormal_compare=", gnCompareIni), value("gnormal_reflect=", gnReflectIni), value("ao_sky=", skyIni), value("ao_sky_range=", skyRangeIni), value("gloss_min=", glossMinIni), value("gloss=", glossIni), value("tess=", tessIni), value("ao_motion=", aoMotionIni), value("denoise_dual=", dualIni), value("ao_ign=", ignIni), value("ao_sunray=", sunRayIni), value("fov_force=", fovForceIni);
                if (l.rfind("frame_trace=", 0) == 0)
                    traceIni = ft != 0;
                if (l.rfind("jitter_log=", 0) == 0)
                    live.jitterLog = jl != 0;
            }
            std::fclose(f);
            live.frameTrace = traceIni;
            live.reflectUp = reflectUpIni != 0;
            live.hudlessOn = hudlessIni != 0, live.hudlessMode = uint32_t(hudlessIni);
            live.fgShift = std::clamp(fgShiftIni, 0.0f, 1.0f);
            live.geoNewVerts = uint32_t(std::clamp(geoNewIni, 0.0f, float(kLiveGeoFrameVerts))), live.geoRefreshVerts = uint32_t(std::clamp(geoRefreshIni, 0.0f, float(kLiveGeoFrameVerts)));
            live.aoBoost = aoBoostIni != 0, live.slowRays = slowRaysIni != 0;
            live.reflTarget = std::max(0.0f, reflMsIni), live.waterOnly = reflectAllIni == 0, live.reflStatsOn = reflStatsIni != 0;
            live.waterBaseOn = std::clamp(waterBaseIni, 0.0f, 1.0f), live.waterGain = std::clamp(waterGainIni, 1.0f, 8.0f);
            live.radOn = radIni != 0, live.radVoxel = std::clamp(radVoxelIni, 0.001f, 0.05f);
            live.lagOn = lagIni != 0, live.lagBias = std::clamp(lagBiasIni, -4.0f, 4.0f);
            live.reflFull = reflFullIni != 0;
            live.denoiseLenOn = denoiseLenIni != 0;
            live.reflSoft = std::clamp(reflSoftIni, 0.0f, 4.0f);
            live.reflSpatial = std::clamp(reflSpatialIni, 0.0f, 1.0f);
            live.reflCoherent = reflCoherentIni != 0;
            live.geoProbe = uint64_t(std::clamp(geoProbeIni, 0.0f, 8000000.0f));
            live.sunEighth = sunEighthIni != 0;
            live.denoiseFarSkip = denoiseFarIni != 0;
            live.gnSlot = int(gnSlotIni), live.gnMode = uint32_t(std::clamp(gnModeIni, 0.0f, 2.0f)), live.gnSigns = uint32_t(gnSignsIni) & 7;
            live.gnUse = gnUseIni != 0, live.gnCompare = gnCompareIni != 0, live.gnReflect = gnReflectIni != 0;
            live.skyAmount = std::clamp(skyIni, 0.0f, 1.0f), live.skyRange = std::clamp(skyRangeIni, 1.0f, 40.0f);
            live.aoMotionFull = aoMotionIni != 0, live.denoiseDual = dualIni != 0, live.aoIgn = ignIni != 0, live.aoSunRay = sunRayIni != 0;
            live.fovForce = (fovForceIni >= 15.0f && fovForceIni <= 80.0f) ? fovForceIni : 0.0f;
            if (live.fovForce > 0 && live.p11 > 0 && live.p00 > 0)
            {
                const float aspect = live.p11 / live.p00;
                live.p11 = 1.0f / std::tan(live.fovForce * 0.5f / 57.2957795f), live.p00 = live.p11 / aspect;
                live.fovMeasured = true, live.fovReset = true;
                Log("URTI live: field of view imposed: %.1f°", live.fovForce);
            }
            live.glossMin = std::clamp(glossMinIni, 0.0f, 0.95f), live.glossAmount = std::clamp(glossIni, 0.0f, 1.0f);
            g_liveTraceOn = traceIni;
            // (with the game's camera only: otherwise nothing changes)
            live.frameSync = frameSyncIni != 0 && g_camCbvSize != 0;
            g_liveFrameSync = live.frameSync;
            live.iniCombine = int(combine); // -1: the mode comes from the ReShade menu (UrtiMode)
            g_liveShowMask = mask != 0, g_liveDebugGame = uint32_t(debug);
            g_liveGameBias = gameBias, g_liveRayBias = rayBias, g_liveSunRadius = sunRadius;
            live.reflStrength = refl, live.reflChannel = uint32_t(channel) & 3, live.reflInvert = invert != 0, live.smoothMin = smoothMin;
            live.f0 = f0, live.debugReflect = uint32_t(debugReflect), live.materialSlot = uint32_t(slot) & 7, live.reflEverywhere = everywhere != 0;
            live.shadowRays = std::clamp(uint32_t(rays), 1u, 4u);
            live.iniDebugView = int(debugView);
            live.reflAlphaMin = std::clamp(reflAlphaMin, 0.01f, 1.0f), live.reflAlphaMax = std::clamp(reflAlphaMax, live.reflAlphaMin, 1.0f);
            live.reflGamma = reflGamma, live.reflDepthTol = reflDepthTol, live.anchorAlpha = anchorAlpha, live.anchorGamma = anchorGamma;
            live.alphaScreen = alphaScreen != 0;
            live.blasRefit = uint32_t(std::max(0.0f, blasRefit));
            if (g_livePersist != (persist != 0))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_livePersist = persist != 0;
                g_livePlaces.clear(), g_liveFreeSpace.clear(), g_liveFreeCounts.clear(), g_livePlaceHigh = 0, g_liveNextCount = 0;
            }
            live.budgetMax = uint64_t(std::max(0.0f, vertexBudget)), live.probeBudget = uint64_t(std::max(0.0f, probeBudget));
            // geometry cache: the frame's scene no longer has to carry everything (near and moving objects)
            live.geoOn = geoIni != 0;
            live.motionLog = motionLogIni != 0;
            live.cameraOn = cameraIni != 0;
            g_liveCensusOn = live.motionLog;
            live.queueSyncOn = queueSyncIni != 0;
            g_liveTraceVertices = uint32_t(traceDraw);
            live.reflectDiag = uint32_t(reflectDiag);
            live.depthLawFixed = int(depthLawFixedIni);
            live.geoWantBytes = uint64_t(std::clamp(geoMb, 64.0f, 8000.0f)) * 1048576;
            if (live.geoOn && live.budgetMax > 1500000)
                live.budgetMax = 1500000;
            if (live.budgetMax == 0 || live.buildTarget <= 0 || live.vertexBudget == 0 || live.vertexBudget > live.budgetMax)
                live.vertexBudget = live.budgetMax;
            live.slotLimit = uint64_t(std::max(0.0f, slotLimit));
            live.uiPhoto = uiPhoto != 0;
            live.nrdShadows = nrdOn != 0, live.nrdDisocclusion = nrdDisocclusion, live.nrdFrames = nrdFrames;
            live.jitterMode = uint32_t(std::clamp(jitter, 0.0f, 2.0f));
            live.fovAuto = fovAuto != 0;
            live.sunMeasure = sunMeasure != 0;
            std::copy(std::begin(zone), std::end(zone), std::begin(live.uiZone));
            live.gameDilate = uint32_t(std::clamp(gameDilate, 0.0f, 3.0f));
            live.gameMin = gameMin != 0;
            live.aoAmount = std::clamp(ao, 0.0f, 1.0f), live.aoRadius = std::clamp(aoRadius, 0.1f, 10.0f);
            live.sunShadows = sunShadows != 0;
            live.aoQuarter = aoQuarter != 0, live.buildTarget = std::max(0.0f, buildMs), live.surfCacheOn = surfCache != 0;
            live.aoTarget = std::max(0.0f, aoMs);
            live.giStrength = std::clamp(gi, 0.0f, 4.0f);
            live.personMask = personMask != 0;
            live.renderAreaOn = renderArea != 0;
            live.waterOn = water != 0;
            live.aoRough = aoRoughIni != 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_liveTranspOn = live.waterOn;
                g_liveTessOn = tessIni != 0;
            }
            live.motionSource = uint32_t(std::clamp(motionSource, 0.0f, 2.0f));
            if (live.motionSource == 2 && !live.mvSynthetic)
                live.mvSynthetic = true, live.mvConvention = 0;
            else if (live.motionSource == 1 && live.mvSynthetic)
                live.mvSynthetic = false, live.mvConvention = 255;
            if (live.aoTarget <= 0)
                live.aoRays = 4;
        }
        if (!first)
            Log("URTI live: settings read again (combine %d, mask %d, check %u, game_bias %.4f, ray_bias %.4f, sun %.2f°; reflections %.2f, "
                "channel %u%s, smoothness min %.2f, f0 %.3f, check %u, target %u)", live.iniCombine, g_liveShowMask, g_liveDebugGame, g_liveGameBias,
                g_liveRayBias, g_liveSunRadius, live.reflStrength, live.reflChannel, live.reflInvert ? " inverted" : "", live.smoothMin, live.f0,
                live.debugReflect, live.materialSlot);
    }
    struct Hot { const char* name; ID3D12RootSignature* rs; ComPtr<ID3D12PipelineState>* pso; };
    const Hot hot[7] = { { "live_shadow", live.shadowRs.Get(), std::addressof(live.shadowPso) },
                         { "live_radcache", live.radRs.Get(), std::addressof(live.radPso) },
                         { "live_reflresolve", live.resolveRs.Get(), std::addressof(live.resolvePso) },
                         { "live_composite", live.compositeRs.Get(), std::addressof(live.compositePso) },
                         { "live_reflect", live.reflectRs.Get(), std::addressof(live.reflectPso) },
                         { "live_surfcache", live.surfRs.Get(), std::addressof(live.surfPso) },
                         { "live_shadowtemporal", live.denoiseRs.Get(), std::addressof(live.denoisePso) } };
    for (int i = 0; i < 7; i++)
    {
        const std::string path = g_dir + "\\urti_shaders\\" + hot[i].name + ".cso";
        const FILETIME t = FileTime(path);
        if (!Newer(t, live.shaderTime[i]) || !hot[i].rs)
            continue;
        live.shaderTime[i] = t;
        std::vector<char> code;
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            code.resize(size_t(std::ftell(f)));
            std::fseek(f, 0, SEEK_SET);
            code.resize(std::fread(code.data(), 1, code.size(), f));
            std::fclose(f);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = hot[i].rs;
        cd.CS = { code.data(), code.size() };
        ComPtr<ID3D12PipelineState> pso;
        if (code.empty() || FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso))))
        {
            Log("URTI live: shader %s refused (invalid signature or code), the old one stays", hot[i].name);
            continue;
        }
        live.retiredPso[live.frame % 3].push_back(*hot[i].pso);
        *hot[i].pso = pso;
        Log("URTI live: shader %s hot-reloaded", hot[i].name);
    }
}

// copy of a G-buffer target (32-bit texels) into a buffer, at the end of its writing (render target
// -> read transition), inside the game's command list
void LiveCopyGbuffer(command_list* cmd, uint64_t target, uint32_t count, const resource* resources, const resource_usage* oldStates,
                     const resource_usage* newStates, ComPtr<ID3D12Resource>& buf, uint64_t& bytes, uint32_t& pitch, uint32_t& width,
                     uint32_t& height, uint32_t& format, uint64_t& frame, const char* what, bool* computeSeen = nullptr, int32_t* copied = nullptr)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (resources[i].handle != target)
            continue;
        const uint32_t before = PassD3D12State(static_cast<uint32_t>(oldStates[i])), after = PassD3D12State(static_cast<uint32_t>(newStates[i]));
        // (computeSeen: also at the end of a compute write, and then only there: some games finish their
        // material target that way, and only there is water 0 in it; the copy at the end of the next frame's
        // G-buffer overwrote it before the reflection computation, and water was seen only one frame in ten)
        const uint32_t written = D3D12_RESOURCE_STATE_RENDER_TARGET | (computeSeen ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : 0);
        if (!(before & written) || (after & written))
            continue;
        if (computeSeen)
        {
            if (before & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                *computeSeen = true;
            else if (*computeSeen)
                continue;
        }
        ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(target);
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        if (PassTexelBytes(uint32_t(d.Format)) != 4 || d.SampleDesc.Count != 1)
            return;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!buf || (copied ? buf->GetDesc().Width : bytes) < total)
        {
            Retire(buf);
            // (common state for a ring buffer: it moves from one queue to the other without a barrier)
            buf = UavBuffer(g.device, total, copied ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST);
            bytes = total;
            Log("URTI live: G-buffer %s %llux%u format %d copied every frame", what, (unsigned long long)d.Width, d.Height, int(d.Format));
        }
        if (!buf)
            return;
        ID3D12GraphicsCommandList* list = Native(cmd);
        Transition(list, res, D3D12_RESOURCE_STATES(after), D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = buf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(after));
        pitch = fp.Footprint.RowPitch, width = uint32_t(d.Width), height = d.Height, format = uint32_t(d.Format), frame = live.frame;
        if (copied)
            *copied = 1;
        return;
    }
}

void LiveGbufferBarrier(command_list* cmd, uint32_t count, const resource* resources, const resource_usage* oldStates, const resource_usage* newStates)
{
    if (!g_liveCapture || !live.enabled || !OurDevice(cmd->get_device()))
        return;
    // end of writing of the main depth: copied here, inside the game's list, with a marker to close the window at execution
    if (live.frameSync)
    {
        uint64_t mainDepth = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            mainDepth = g_liveMainDepth;
        }
        for (uint32_t i = 0; i < count && mainDepth; i++)
        {
            if (resources[i].handle != mainDepth)
                continue;
            const uint32_t before = PassD3D12State(static_cast<uint32_t>(oldStates[i])), after = PassD3D12State(static_cast<uint32_t>(newStates[i]));
            if (!(before & D3D12_RESOURCE_STATE_DEPTH_WRITE) || (after & D3D12_RESOURCE_STATE_DEPTH_WRITE))
                continue;
            ID3D12GraphicsCommandList* l = Native(cmd);
            if (l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
                break;
            ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(mainDepth);
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
            std::lock_guard<std::mutex> lock(g_mutex);
            if (live.depthSnapBytes < total)
            {
                for (auto& b : live.depthSnap)
                    Retire(b), b = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COMMON);
                live.depthSnapBytes = total;
                Log("URTI live: main depth %llux%u format %d copied inside the game's lists (%u buffers of %.1f MB)", (unsigned long long)d.Width, d.Height, int(d.Format),
                    LiveState::kDepthSnaps, double(total) / 1048576.0);
            }
            // nothing drawn since the last copy: the same buffer will do
            const bool again = g_liveMainAtCopy != g_liveMainRecorded;
            const uint32_t k = again ? g_liveSnapNext++ % LiveState::kDepthSnaps : (g_liveSnapNext + LiveState::kDepthSnaps - 1) % LiveState::kDepthSnaps;
            if (!live.depthSnap[k])
                break;
            if (again)
            {
                g_liveMainAtCopy = g_liveMainRecorded;
                g_liveSnapCopies++;
                // (the depth plane only: the stencil's may be in another state. Already readable by a copy, as
                // when the game copies its depth itself: no barrier, a transition from a state to itself loses the GPU)
                const bool readable = (after & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = res;
                b.Transition.Subresource = 0;
                b.Transition.StateBefore = D3D12_RESOURCE_STATES(after);
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                if (!readable)
                    l->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
                dst.pResource = live.depthSnap[k].Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = fp;
                src.pResource = res;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                if (!readable)
                    l->ResourceBarrier(1, &b);
            }
            live.depthSnapPitch = fp.Footprint.RowPitch, live.depthSnapWidth = uint32_t(d.Width), live.depthSnapHeight = d.Height;
            LivePending mark;
            mark.depthMark = int32_t(k);
            mark.depthAfter = after;
            mark.mainBefore = g.lists[uint64_t(l)].mainDraws;
            mark.window = g_liveWindow;
            g_livePending[uint64_t(l)].push_back(mark);
            break;
        }
    }
    // the game's image without its UI: screen-sized RGBA 16-bit float images at the end of their writing (marker
    // to recognise it at execution); the adopted one is copied here, into a ring
    if (live.hudlessOn && live.frameSync)
    {
        uint32_t bw = 0, bh = 0;
        uint64_t adopted = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            bw = g_liveHudW, bh = g_liveHudH, adopted = g_liveHudHandle;
        }
        for (uint32_t i = 0; i < count && bw; i++)
        {
            if (!resources[i].handle)
                continue;
            const uint32_t before = PassD3D12State(static_cast<uint32_t>(oldStates[i])), after = PassD3D12State(static_cast<uint32_t>(newStates[i]));
            const uint32_t written = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (!(before & written) || (after & written))
                continue;
            ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(resources[i].handle);
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != bw || d.Height != bh || d.SampleDesc.Count != 1 || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
                (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS))
                continue;
            ID3D12GraphicsCommandList* l = Native(cmd);
            if (!l || l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
                break;
            LivePending mark;
            mark.hudHandle = resources[i].handle;
            if (resources[i].handle == adopted)
            {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
                UINT64 total = 0;
                g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
                std::lock_guard<std::mutex> lock(g_mutex);
                if (live.hudBytes < total)
                {
                    for (auto& b : live.hudSnap)
                        Retire(b), b = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COMMON);
                    live.hudBytes = total;
                    Log("URTI live: the game's UI-free image %llux%u copied inside its lists (%u buffers of %.1f MB)", (unsigned long long)d.Width, d.Height,
                        LiveState::kHudSnaps, double(total) / 1048576.0);
                }
                const uint32_t k = g_liveHudNext++ % LiveState::kHudSnaps;
                if (live.hudSnap[k])
                {
                    // (already readable by a copy: no barrier, a transition from a state to itself loses the GPU)
                    const bool readable = (after & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
                    if (!readable)
                        Transition(l, res, D3D12_RESOURCE_STATES(after), D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
                    dst.pResource = live.hudSnap[k].Get();
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint = fp;
                    src.pResource = res;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    if (!readable)
                        Transition(l, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(after));
                    live.hudPitch = fp.Footprint.RowPitch, live.hudWidth = uint32_t(d.Width), live.hudHeight = d.Height;
                    mark.hudMark = int32_t(k);
                }
                mark.window = g_liveWindow;
                g_livePending[uint64_t(l)].push_back(mark);
                continue;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            mark.window = g_liveWindow;
            g_livePending[uint64_t(l)].push_back(mark);
        }
    }
    uint64_t target = 0, motionTarget = 0, albedoTarget = 0, normalTarget = 0, gnTarget = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (live.gnMode && live.gnSlot >= 0 && uint32_t(live.gnSlot) < g_liveGbufferCount)
            gnTarget = g_liveGbuffer[live.gnSlot];
        if (live.materialSlot < g_liveGbufferCount && live.reflStrength > 0)
            target = g_liveGbuffer[live.materialSlot];
        // the surfaces' own colour (first G-buffer target): light bounce
        if (g_liveGbufferCount >= 3 && live.giStrength > 0 && live.aoAmount > 0)
            albedoTarget = g_liveGbuffer[0], normalTarget = live.personMask ? g_liveGbuffer[1] : 0;
        // motion vectors: a G-buffer target with two 16-bit floats (the n-th if there are several)
        uint64_t candidates[8] = {};
        uint32_t found = 0;
        for (uint32_t k = 0; k < g_liveGbufferCount && k < 8; k++)
            if (g_liveGbuffer[k])
            {
                const DXGI_FORMAT f = reinterpret_cast<ID3D12Resource*>(g_liveGbuffer[k])->GetDesc().Format;
                if (f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_TYPELESS)
                    candidates[found++] = g_liveGbuffer[k];
            }
        if (found && !live.mvSynthetic)
            motionTarget = candidates[live.mvCandidate % found];
    }
    for (uint32_t i = 0; i < count && motionTarget; i++)
    {
        if (resources[i].handle != motionTarget)
            continue;
        const uint32_t before = PassD3D12State(static_cast<uint32_t>(oldStates[i])), after = PassD3D12State(static_cast<uint32_t>(newStates[i]));
        if (!(before & D3D12_RESOURCE_STATE_RENDER_TARGET) || (after & D3D12_RESOURCE_STATE_RENDER_TARGET))
            continue;
        ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(motionTarget);
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!live.mvBuf || live.mvBytes < total)
        {
            Retire(live.mvBuf);
            live.mvBuf = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
            live.mvBytes = total;
            Log("URTI live: the game's motion vectors %llux%u copied every frame (accumulation over time)", (unsigned long long)d.Width, d.Height);
        }
        if (!live.mvBuf)
            break;
        ID3D12GraphicsCommandList* l = Native(cmd);
        Transition(l, res, D3D12_RESOURCE_STATES(after), D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = live.mvBuf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(l, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(after));
        live.mvPitch = fp.Footprint.RowPitch, live.mvWidth = uint32_t(d.Width), live.mvHeight = d.Height, live.mvFrame = live.frame;
        break;
    }
    if (target && live.frameSync)
    {
        // into a ring, with a marker: the computation takes the one of its frame, and never reads a buffer being written
        uint32_t k = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            k = g_liveMaterialNext % LiveState::kMaterialSnaps;
        }
        int32_t copied = 0;
        LiveCopyGbuffer(cmd, target, count, resources, oldStates, newStates, live.materialSnap[k], live.materialBytes, live.materialPitch,
                        live.materialWidth, live.materialHeight, live.materialFormat, live.materialFrame, "material (target 2)", &live.materialAfterCompute, &copied);
        if (copied)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_liveMaterialNext++;
            LivePending mark;
            mark.materialMark = int32_t(k);
            mark.window = g_liveWindow;
            g_livePending[uint64_t(Native(cmd))].push_back(mark);
        }
    }
    else if (target)
        LiveCopyGbuffer(cmd, target, count, resources, oldStates, newStates, live.materialBuf, live.materialBytes, live.materialPitch,
                        live.materialWidth, live.materialHeight, live.materialFormat, live.materialFrame, "material (target 2)", &live.materialAfterCompute);
    // G-buffer normals: at the end of their writing; with frame_sync, in the ring slot the same frame's material will take
    if (gnTarget && live.frameSync)
    {
        uint32_t k = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            k = g_liveMaterialNext % LiveState::kMaterialSnaps;
        }
        int32_t gnCopied = 0;
        LiveCopyGbuffer(cmd, gnTarget, count, resources, oldStates, newStates, live.gnSnap[k], live.gnBytes, live.gnPitch,
                        live.gnWidth, live.gnHeight, live.gnFormat, live.gnFrame, "normals (gnormal_slot setting)", nullptr, &gnCopied);
    }
    else if (gnTarget)
        LiveCopyGbuffer(cmd, gnTarget, count, resources, oldStates, newStates, live.gnBuf, live.gnBytes, live.gnPitch,
                        live.gnWidth, live.gnHeight, live.gnFormat, live.gnFrame, "normals (gnormal_slot setting)");
    if (albedoTarget)
        LiveCopyGbuffer(cmd, albedoTarget, count, resources, oldStates, newStates, live.albedoBuf, live.albedoBytes, live.albedoPitch,
                        live.albedoWidth, live.albedoHeight, live.albedoFormat, live.albedoFrame, "own colour (albedo, target 0)");
    if (normalTarget)
        LiveCopyGbuffer(cmd, normalTarget, count, resources, oldStates, newStates, live.normalBuf, live.normalBytes, live.normalPitch,
                        live.normalWidth, live.normalHeight, live.normalFormat, live.normalFrame, "normale (cible 1)");
}

// detailed timing: marker k of the computation (0 to 11, frame ring) or of the composition (12 to 17, present ring)
void LiveTimeD(ID3D12GraphicsCommandList* list, uint32_t k)
{
    if (live.timeDHeap)
        list->EndQuery(live.timeDHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, uint32_t(k < 12 ? live.frame % 3 : live.compCounter % 3) * LiveState::kTimeD + k);
}

// timestamp k (0 to 7) of the current frame
void LiveTime(ID3D12GraphicsCommandList* list, uint32_t k)
{
    if (live.timeHeap)
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (live.frame % 3) * 8 + k);
}

// geometry cache cleared (camera cut, another main screen): its frame is worthless
// The game's camera for this frame -> pose from the cache's frame to the camera and motion since the previous frame, to write into
// motion (same places as live_camfit; bytes 176 to 183: sub-pixel jitter). URTI's camera space: x = right . d,
// y = up . d, z = w = -back . d, d = point - position. False: no game camera, the old measurement goes on.
void LiveGeoFlush(const char* why);

bool LiveCameraFrame(float* out)
{
    live.camLive = live.cameraOn && live.camSerialNow != 0;
    // camera lost (the game no longer writes it there, urti_camscan.inl looks for another one): the old measurement takes over. Otherwise the last camera stays, frozen
    // while the player moves: zero motion, history read at the wrong place, kept objects misplaced (pixel soup while running)
    {
        const uint32_t latest = g_camSerial.load(std::memory_order_acquire);
        if (latest != live.camSeenSerial)
            live.camSeenSerial = latest, live.camSeenFrame = live.frame;
        const bool lost = live.frame > live.camSeenFrame + 30;
        if (lost != live.camLost)
        {
            live.camLost = lost;
            Log("URTI live: game camera %s", lost ? "lost: measuring on the witnesses instead" : "read again");
            LiveGeoFlush(lost ? "game camera lost" : "game camera found again");
        }
        if (lost)
            live.camLive = false;
    }
    if (!live.camLive)
        return false;
    const GameCamera prev = live.camCur;
    const bool havePrev = live.camHaveCur;
    if (live.camSerialNow != live.camSerialDone)
    {
        live.statCamNew += live.camSerialDone ? live.camSerialNow - live.camSerialDone : 0;
        live.camCur = CamGet(live.camSerialNow), live.camSerialDone = live.camSerialNow, live.camHaveCur = true;
    }
    const GameCamera& c = live.camCur;
    if (!live.camHaveCur || c.p00 <= 0)
        return false;
    live.p00 = c.p00, live.p11 = c.p11;
    live.A = -c.a, live.B = c.b;
    if (live.geoResetPose || !live.camHaveRef)
        live.camRef = c, live.camHaveRef = true, live.geoResetPose = false;
    const auto rows = [](const GameCamera& k, double m[3][3]) {
        for (int i = 0; i < 3; i++)
            m[0][i] = k.right[i], m[1][i] = k.up[i], m[2][i] = -double(k.back[i]);
    };
    // A = Rcur Ra^T, t = Rcur (posa - poscur): point of a's camera space -> this frame's camera space
    const auto relative = [&](const GameCamera& a, float* rOut, float* tOut, double* rotDeg, double* move) {
        double rc[3][3], ra[3][3];
        rows(c, rc), rows(a, ra);
        double trace = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
            {
                double v = 0;
                for (int k = 0; k < 3; k++)
                    v += rc[i][k] * ra[j][k];
                rOut[i * 3 + j] = float(v);
                if (i == j)
                    trace += v;
            }
        const double d[3] = { double(a.pos[0]) - c.pos[0], double(a.pos[1]) - c.pos[1], double(a.pos[2]) - c.pos[2] };
        for (int i = 0; i < 3; i++)
            tOut[i] = float(rc[i][0] * d[0] + rc[i][1] * d[1] + rc[i][2] * d[2]);
        if (rotDeg)
            *rotDeg = std::acos(std::clamp((trace - 1.0) * 0.5, -1.0, 1.0)) * 57.29578;
        if (move)
            *move = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    };
    double rotDeg = 0, move = 0;
    relative(havePrev ? prev : c, out + 0, out + 9, &rotDeg, &move);
    out[12] = 1.0f; // valide
    out[13] = out[14] = out[15] = 0.0f;
    relative(live.camRef, out + 32, out + 41, nullptr, &live.geoPoseDistance);
    // sub-pixel jitter: x of the captured space = true x - (jx / p00) w
    out[44] = -c.jx / c.p00, out[45] = -c.jy / c.p11;
    // world vertical axis: the one where the camera's up weighs most and its right least (the camera does not tilt); water is flat and
    // horizontal, its exact normal is better than the one from depth (shifted every frame: the reflection shook)
    for (int k = 0; k < 3; k++)
        live.camUpScore[k] += std::fabs(c.up[k]) - std::fabs(c.right[k]);
    {
        const int axis = live.camUpScore[0] > live.camUpScore[1] ? (live.camUpScore[0] > live.camUpScore[2] ? 0 : 2) : (live.camUpScore[1] > live.camUpScore[2] ? 1 : 2);
        const float sign = c.up[axis] >= 0 ? 1.0f : -1.0f;
        float n[3] = { c.right[axis] * sign, c.up[axis] * sign, -c.back[axis] * sign };
        // (in the space shifted by the sub-pixel jitter, like the geometry)
        n[2] -= out[44] * n[0] + out[45] * n[1];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        live.worldUp[0] = n[0] / len, live.worldUp[1] = n[1] / len, live.worldUp[2] = n[2] / len;
        live.worldUpKnown = true;
        live.worldUpAxis = axis, live.worldUpSign = sign;
    }
    live.statCamFrames++, live.statCamTrans += move, live.statCamRot += rotDeg;
    live.statCamTransMax = std::max(live.statCamTransMax, move);
    if (move > 5.0)
        live.statCamJumps++;
    if (live.statCamFrames >= 300)
    {
        Log("URTI live: game camera: %u frames, %u new, %.2f cm and %.3f° per frame on average, %.1f cm at most, %u jumps over 5 m; field %.2f°, jitter (%.5f, %.5f), law %.4g + %.5f / w; %llu read, %llu refused",
            live.statCamFrames, live.statCamNew, live.statCamTrans / live.statCamFrames * 100.0, live.statCamRot / live.statCamFrames, live.statCamTransMax * 100.0, live.statCamJumps,
            2.0 * std::atan(1.0 / c.p11) * 57.29578, c.jx, c.jy, live.A, live.B, (unsigned long long)g_camProbes.load(), (unsigned long long)g_camRefused.load());
        live.statCamFrames = live.statCamNew = live.statCamJumps = 0, live.statCamTrans = live.statCamRot = live.statCamTransMax = 0;
    }
    return true;
}

void LiveGeoFlush(const char* why)
{
    if (!live.geo.empty())
        Log("URTI live: geometry cache cleared (%s): %zu objects", why, live.geo.size());
    live.geo.clear(), live.geoByOffset.clear(), live.meshProbed.clear();
    live.geoHead = 0, live.geoResetPose = true, live.geoLastFlush = live.frame;
    live.sentinels.clear(), live.sentinelRef.clear(), live.sentinelFree.clear();
    live.statGeoFlushes++;
}

// main screen: the screen-sized depth drawn the most, over the last frames (a capture window
// may hold only part of a game frame: without this average, a window without draws of the real
// depth chose another one for a frame, and the computation started from the wrong one: no effect at all on
// that frame)
int LiveMainSlot()
{
    int best = -1;
    float most = 0.0f;
    for (uint32_t s = 0; s < kSlots; s++)
        if (g.slotDepth[s] && g.screenLike[g.slotDepth[s]] == 1)
            if (const float score = g.slotScore[s] + float(g.slotDraws[s]); score >= most)
                most = score, best = int(s);
    return best;
}

double LiveTraceMs()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

void OnBeginEffects(effect_runtime* runtime, command_list* cmd, resource_view, resource_view)
{
    if (!OurDevice(runtime->get_device()))
        return;
    if (live.toggleSplit.exchange(false))
        if (const effect_uniform_variable split = runtime->find_uniform_variable("urti.fx", "UrtiSplit"); split.handle)
        {
            bool on = false;
            runtime->get_uniform_value_bool(split, &on, 1);
            runtime->set_uniform_value_bool(split, !on);
            Log("URTI: split screen %s", on ? "off" : "on");
        }
    if (!live.enabled)
    {
        // shadows off: the effect must no longer apply the old shadow image
        if (const effect_uniform_variable off = runtime->find_uniform_variable("urti.fx", "UrtiActive"); off.handle)
            runtime->set_uniform_value_bool(off, false);
        live.bound = false;
        return;
    }
    if (g.phase != Phase::Idle)
        return;
    live.statEffects++;
    ID3D12Device5* device5 = nullptr;
    if (FAILED(g.device->QueryInterface(IID_PPV_ARGS(&device5))))
        return;
    ComPtr<ID3D12Device5> device(device5);
    device5->Release();
    ComPtr<ID3D12GraphicsCommandList4> list;
    if (FAILED(Native(cmd)->QueryInterface(IID_PPV_ARGS(&list))))
        return;
    {
        std::lock_guard<std::mutex> lock(g_liveRetireMutex);
        live.retiredLong.erase(std::remove_if(live.retiredLong.begin(), live.retiredLong.end(), [](const LiveState::RetiredBuffer& b) { return live.frame > b.frame + 120; }), live.retiredLong.end());
    }
    live.retiredPso[live.frame % 3].clear();
    // everything previous presents sent to this queue is finished when this fence passes: protects the ring buffers
    // (see below), rewritten three presents later
    ID3D12CommandQueue* slotQueue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
    if (!live.slotFence && SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&live.slotFence))))
        live.slotEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (live.slotFence && SUCCEEDED(slotQueue->Signal(live.slotFence.Get(), ++live.slotFenceValue)))
        live.slotDone[(live.frame + 2) % 3] = live.slotFenceValue;
    if (!live.convertPso && !LiveCreatePipelines(g.device))
    {
        Log("URTI live: shaders refused, live mode off");
        live.enabled = false;
        return;
    }
    LiveHotReload(g.device);
    if (live.iniCombine >= 0)
        g_liveCombine = live.iniCombine != 0;
    else
        g_liveCombine = true; // only the shadows the game lacks (the menu only has "URTI" left)

    // what the stream received three frames ago (read without waiting)
    if (const uint64_t* c = nullptr; live.counterReadback[live.frame % 3] && SUCCEEDED(live.counterReadback[live.frame % 3]->Map(0, nullptr, (void**)&c)) && c)
    {
        const int slot = LiveMainSlot();
        if (slot >= 0)
            live.lastFilled = c[slot] / 16;
        live.counterReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    const int slot = LiveMainSlot();
    const uint32_t drawsThisFrame = slot >= 0 ? uint32_t(g.slotDraws[slot]) : 0;
    std::vector<LiveMark> marksNow;
    std::vector<LiveRegion> regionsNow, probesNow, placesNow;
    // start in the stream (bytes) of the region written by each draw of this frame, in regionsNow's order;
    // of each place, in placesNow's order (a place kept without being drawn: not read)
    std::vector<uint32_t> srcNow, placeSrc;
    std::vector<LiveRegion> transpNow; // transparent surfaces executed (layer 2) and their start in the stream
    std::vector<uint32_t> transpSrcNow;
    uint32_t transpCursorNow = 0;
    uint32_t cursorNow = 0, zeroArea = ~0u;
    bool persistNow = false;
    // one game frame at a time: computation only when one more frame is complete; no marker for 16 presents (screen
    // without the main depth, menu): the old rule
    bool snapNew = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (live.frameSync && g_liveSnapSerial != live.snapSerialSeen)
            live.snapSerialSeen = g_liveSnapSerial, live.snapSeenFrame = live.frame;
        live.syncNow = live.frameSync && g_liveSnapSerial != 0 && live.frame <= live.snapSeenFrame + 16;
        snapNew = g_liveSnapSerial != live.snapSerialUsed;
    }
    // its material not executed yet: on the next present (two presents at most)
    if (live.syncNow && snapNew && live.reflStrength > 0 && live.frame <= live.snapSeenFrame + 2)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // (next frame already waiting: no wait, the lag must not build up)
        if (g_liveWindowMaterial < 0 && g_liveMaterialDone >= 0 && g_liveDeferred.empty())
            snapNew = false, live.statMaterialWait++;
    }
    live.presentComputed = false;
    {
        // game hitch: a present arriving more than 2.2 times later than usual; the next three computations are catch-ups
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now), QueryPerformanceFrequency(&freq);
        const double dt = live.lastPresentTime.QuadPart ? double(now.QuadPart - live.lastPresentTime.QuadPart) * 1000.0 / double(freq.QuadPart) : 0.0;
        live.lastPresentTime = now;
        if (dt > 0 && dt < 500.0)
        {
            if (live.presentMs > 0 && dt > 2.2 * live.presentMs)
                live.suspectLeft = 3;
            live.presentMs = live.presentMs > 0 ? live.presentMs + (std::min(dt, 3.0 * live.presentMs) - live.presentMs) * 0.05 : dt;
        }
    }
    if ((live.syncNow ? !snapNew : live.lastPresentDraws == 0) && live.bound)
    {
        // no game draw executed since the previous present (frame generated by DLSS-G or FSR3, or a game
        // that presents twice): nothing new to compute, the last result is laid again; the capture window
        // does not advance (draws executed afterwards belong to the game's next frame)
        live.statNoDraw++;
        if (live.frameTrace)
            Log("FRAME %llu t %.2f no compute gen %d", (unsigned long long)live.frame, LiveTraceMs(), int(live.imageGenerated));
        live.frame++;
        return;
    }
    live.presentComputed = true;
    {
        // catch-up computation (right after a game hitch: several complete frames waiting): its layer is not shown (layer queue)
        // computation whose frame has almost no game draw executed before this present (less than a tenth of usual: the game's lists
        // went into the previous window): its layer differed from the others (reflection partly missing) and showed for one present
        const double pd = double(live.lastPresentDraws);
        const bool starved = live.presentDrawsAvg > 200.0 && pd < 0.1 * live.presentDrawsAvg;
        if (!starved)
            live.presentDrawsAvg = live.presentDrawsAvg > 0 ? live.presentDrawsAvg + (pd - live.presentDrawsAvg) * 0.05 : pd;
        live.layerSuspect = live.suspectLeft > 0 || starved;
        if (live.suspectLeft > 0)
            live.suspectLeft--;
        live.statSuspect += live.layerSuspect ? 1u : 0u;
    }
    // upload and readback buffers in a ring, by frame % 3: if the GPU is more than three presents behind, this ring's
    // slot is still being read: rewriting it would give the GPU wrong region tables (reads outside the buffers, GPU lost)
    if (const uint64_t need = live.slotDone[live.frame % 3]; need && live.slotEvent && live.slotFence->GetCompletedValue() < need)
    {
        live.statSlotWaits++;
        if (SUCCEEDED(live.slotFence->SetEventOnCompletion(need, live.slotEvent)))
            WaitForSingleObject(live.slotEvent, 200);
    }
    // every game draw executed so far finished before this computation: the game executes its lists on a different queue from
    // the present queue, and nothing orders them; what had just been executed (water, late in the frame) had not
    // written its vertices or its counter yet: empty object, blinking reflection. Wait on the GPU, not on the CPU
    if (live.queueSyncOn)
    {
        ID3D12CommandQueue* presentQueue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const uint64_t q : g_liveExecQueues)
        {
            ID3D12CommandQueue* gameQueue = reinterpret_cast<ID3D12CommandQueue*>(q);
            if (gameQueue == presentQueue)
                continue;
            LiveState::QueueSync& sync = live.queueSync[q];
            if (!sync.fence && FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sync.fence))))
                continue;
            sync.value++;
            if (SUCCEEDED(gameQueue->Signal(sync.fence.Get(), sync.value)) && SUCCEEDED(presentQueue->Wait(sync.fence.Get(), sync.value)))
                live.statSyncWaits++;
        }
        g_liveExecQueues.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        marksNow.swap(g_liveMarks);
        {
            // the largest of the frequent enough viewports (a fifth of the draws of the most frequent): passes at
            // half resolution (852×480 in one game) also draw into this depth, a few 3D markers
            // keep the full area
            uint32_t most = 0;
            for (const auto& [extent, n] : g_liveViewCounts)
                most = std::max(most, n);
            uint32_t best = 0;
            uint64_t bestArea = 0;
            for (const auto& [extent, n] : g_liveViewCounts)
                if (n * 5 >= most && uint64_t(extent >> 16) * (extent & 0xFFFF) > bestArea)
                    best = extent, bestArea = uint64_t(extent >> 16) * (extent & 0xFFFF);
            // an occasional frame (environment probe?) is dominated by another area: the rendered area only
            // changes after ten frames in a row with the same new area (dynamic resolution changes slowly)
            if (most >= 16)
            {
                if (best == live.viewCandidate)
                    live.viewStreak++;
                else
                    live.viewCandidate = best, live.viewStreak = 1;
                if (live.viewStreak >= 10 || live.viewMax[0] == 0)
                    live.viewMax[0] = float(best >> 16), live.viewMax[1] = float(best & 0xFFFF);
            }
            if (++live.viewLogCount % 600 == 1 && !g_liveViewCounts.empty())
            {
                std::vector<std::pair<uint32_t, uint32_t>> v(g_liveViewCounts.begin(), g_liveViewCounts.end());
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                char text[256] = "";
                for (size_t i = 0; i < v.size() && i < 5; i++)
                {
                    char item[48];
                    std::snprintf(item, sizeof(item), " %ux%u : %u", v[i].first >> 16, v[i].first & 0xFFFF, v[i].second);
                    std::strncat(text, item, sizeof(text) - std::strlen(text) - 1);
                }
                Log("URTI live: viewports in the screen depth (draws):%s", text);
            }
            g_liveViewCounts.clear();
        }
        if (slot >= 0)
            regionsNow.swap(g_liveRegions[slot]), srcNow.swap(g_liveRegionSrc[slot]), cursorNow = g_liveCursor[slot];
        transpNow.swap(g_liveTranspRegions), transpSrcNow.swap(g_liveTranspSrc), transpCursorNow = g_liveTranspCursor;
        g_liveTranspRegions.clear(), g_liveTranspSrc.clear(), g_liveTranspCursor = 0, g_liveTranspVerts = 0;
        // fixed places: an object absent for more than kLiveKeepFrames frames gives its place back; the others stay
        // in the RT scene with their last geometry; used part too fragmented: everything is redistributed
        if (g_livePersist && slot >= 0 && g.slotDepth[slot] == g_liveMainDepth)
        {
            constexpr uint64_t kLiveKeepFrames = 3;
            uint64_t held = 0;
            for (auto it = g_livePlaces.begin(); it != g_livePlaces.end();)
            {
                if (g_livePlaceFrame - it->second.lastFrame > kLiveKeepFrames)
                {
                    LiveFreePlace(it->second);
                    it = g_livePlaces.erase(it);
                    continue;
                }
                // drawn in this frame: converted from its region of the stream, up to its counter; otherwise kept
                // as it is in the RT scene (counter ~0: the conversion does not touch it)
                const bool drawn = it->second.lastFrame == g_livePlaceFrame;
                placesNow.push_back({ it->second.offset, it->second.capacity, drawn ? it->second.frameCounter : ~0u, it->first });
                held += it->second.capacity;
                ++it;
            }
            std::sort(placesNow.begin(), placesNow.end(), [](const LiveRegion& a, const LiveRegion& b) { return a.offset < b.offset; });
            {
                std::vector<uint32_t> order(regionsNow.size());
                for (uint32_t i = 0; i < order.size(); i++)
                    order[i] = i;
                std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return regionsNow[a].offset < regionsNow[b].offset; });
                std::vector<LiveRegion> r(order.size());
                std::vector<uint32_t> src(order.size());
                for (size_t i = 0; i < order.size(); i++)
                    r[i] = regionsNow[order[i]], src[i] = srcNow[order[i]];
                regionsNow.swap(r), srcNow.swap(src);
            }
            std::unordered_map<uint32_t, uint32_t> srcOf;
            for (size_t i = 0; i < regionsNow.size(); i++)
                srcOf[regionsNow[i].id] = srcNow[i];
            placeSrc.resize(placesNow.size());
            for (size_t i = 0; i < placesNow.size(); i++)
            {
                const auto f = srcOf.find(placesNow[i].id);
                placeSrc[i] = f != srcOf.end() ? f->second : 0;
                // drawn at the same place in the previous frame: its old vertices serve the camera motion
                if (f != srcOf.end())
                    if (const auto pl = g_livePlaces.find(placesNow[i].id);
                        pl != g_livePlaces.end() && pl->second.prevFrame + 1 == g_livePlaceFrame && pl->second.stableOrder && pl->second.uniquePrev &&
                        g_liveKeyRank[pl->second.rankKey] == 1)
                        placeSrc[i] |= 0x80000000u;
            }
            cursorNow = g_livePlaceHigh, persistNow = true;
            live.statPlaces += g_livePlaces.size(), live.statPlaceHeld += held, live.statPlaceHigh += g_livePlaceHigh;
            live.statPlaceNew += g_livePlaceNew, live.statPlaceFrames++;
            live.placesMoved = live.placesResetPending || g_livePlaceNew > placesNow.size() / 16 + 8;
            live.placesResetPending = false;
            // a refused place (slot full), or more than a fifth of the used part lost to holes:
            // every place is given back, the next frame redistributes them one after the other (the structure being
            // rebuilt every frame, only objects kept without being drawn miss one frame)
            if (g_livePlaceFailed || held < uint64_t(g_livePlaceHigh) * 8 / 10)
            {
                live.statPlaceResets++, live.placesResetPending = true;
                g_livePlaces.clear(), g_liveFreeSpace.clear(), g_liveFreeCounts.clear(), g_livePlaceHigh = 0, g_liveNextCount = 0;
            }
            g_livePlaceNew = g_livePlaceFailed = 0;
        }
        else if (!g_livePlaces.empty())
            g_livePlaces.clear(), g_liveFreeSpace.clear(), g_liveFreeCounts.clear(), g_livePlaceHigh = 0, g_liveNextCount = 0;
        g_livePlaceFrame++;
        for (uint32_t s = 0; s < kSlots; s++)
            g_liveRegions[s].clear(), g_liveRegionSrc[s].clear(), g_liveCursor[s] = 0;
        // meshes drawn only once in this frame (for the next frame's pairs)
        for (auto& [id, place] : g_livePlaces)
            if (place.lastFrame == g_livePlaceFrame - 1)
            {
                const auto r = g_liveKeyRank.find(place.rankKey);
                place.uniquePrev = r != g_liveKeyRank.end() && r->second == 1;
            }
        g_liveKeyRank.clear();
        live.camSerialNow = g_liveCommitCam;
        if (slot >= 0)
            probesNow.swap(g_liveProbes[slot]);
        for (uint32_t s = 0; s < kSlots; s++)
            g_liveProbes[s].clear();
        // next window: its part of the stream (used three windows ago) starts again from the beginning; its counters are
        // reset at the end of this frame, before any list is written in the new window
        g_liveWindow++;
        const uint32_t nextArea = uint32_t(g_liveWindow % kLiveAreas);
        for (uint32_t s = 0; s < kSlots; s++)
            g_liveAreaCursor[s][nextArea] = g_liveAreaProbe[s][nextArea] = 0;
        g_liveAreaCounters[nextArea] = 0;
        // depth of this window; lists of the next frame already executed go into the new window (which may already
        // close: one more computation on the next present)
        live.snapIndexNow = g_liveSnapDone, live.snapSerialUsed = g_liveSnapSerial;
        // copy of the UI-free image of this game frame: one per frame, in order; the last known pair gives the number (the copy
        // itself may not be executed yet). The layer is realigned on the displayed image from that number (live_hudgain)
        live.layerHudSerial = g_livePairHud + (g_liveSnapSerial - g_livePairSnap);
        live.statMaterialLate += live.syncNow && g_liveWindowMaterial < 0 ? 1 : 0;
        live.materialIndexNow = g_liveWindowMaterial >= 0 ? g_liveWindowMaterial : g_liveMaterialDone;
        g_liveWindowMaterial = -1;
        g_liveWindowClosed = false;
        {
            std::vector<LiveDeferred> later;
            later.swap(g_liveDeferred);
            // several complete frames waiting (big hitch: the game went on while presents were blocked): only the
            // last one is kept. One computation per frame never catches up its lag: the expected stream regions were reused before being
            // read, and the frame's scene stayed empty forever, with nothing saying so
            {
                struct Close
                {
                    size_t list, entry;
                    uint32_t counted;
                };
                std::vector<Close> closes;
                uint32_t since = 0, last = g_liveMainLast;
                for (size_t i = 0; i < later.size(); i++)
                {
                    uint32_t counted = later[i].counted;
                    for (size_t j = 0; j < later[i].pending.size(); j++)
                    {
                        const LivePending& p = later[i].pending[j];
                        if (p.depthMark < 0)
                            continue;
                        since += p.mainBefore - counted;
                        counted = p.mainBefore;
                        if (since >= 16 && uint64_t(since) * 10 >= uint64_t(last) * 6)
                            closes.push_back({ i, j, counted }), last = since, since = 0;
                    }
                    since += later[i].listMain - counted;
                }
                if (closes.size() >= 2)
                {
                    const Close& c = closes[closes.size() - 2];
                    later[c.list].pending.erase(later[c.list].pending.begin(), later[c.list].pending.begin() + c.entry + 1);
                    later[c.list].counted = c.counted;
                    later.erase(later.begin(), later.begin() + c.list);
                    live.statFramesSkipped += uint32_t(closes.size() - 1);
                }
            }
            for (LiveDeferred& d : later)
                if (g_liveWindowClosed)
                    g_liveDeferred.push_back(std::move(d));
                else
                    LiveCommit(d.pending, d.listMain, d.counted);
        }
        // counters reset one window ahead: this reset runs at the end of this computation, on the present
        // queue, and nothing orders it with the game's queue. For the window that starts, it arrived while
        // the game was already drawing: every counter written before it was erased (random empty objects, water
        // captured one frame in eight, blinking places). The one after starts only after the next computation; its
        // last reads (kLiveAreaMaxAge) are done further up in this frame
        zeroArea = uint32_t((g_liveWindow + 1) % kLiveAreas);
        for (auto it = g_livePending.begin(); it != g_livePending.end();) // lists written but never executed
            it = !it->second.empty() && g_liveWindow - it->second.front().window > 8 ? g_livePending.erase(it) : std::next(it);
        for (const auto& [id, mesh] : g_liveIdMesh)
            live.idMesh[id] = mesh;
        g_liveIdMesh.clear();
        live.stableMeshes.insert(g_liveStableNow.begin(), g_liveStableNow.end());
        g_liveStableNow.clear();
        if (live.stableMeshes.size() > 100000)
            live.stableMeshes.clear();
        if (live.idMesh.size() > 200000)
            live.idMesh.clear();
        live.seenNow.swap(g_liveSeen);
        // (increase limited: a window holding two game frames does not double the meshes' cost: the budget dropped
        // from 105 to 27 objects for one frame, and they came back on the next)
        for (const auto& [mesh, vertices] : live.seenNow)
        {
            auto& seen = live.seenRecent[mesh];
            seen.vertices = seen.frame ? std::min(vertices, seen.vertices + seen.vertices / 4 + 64) : vertices;
            seen.frame = live.frame;
        }
        live.seenNow.clear();
        for (auto it = live.seenRecent.begin(); it != live.seenRecent.end();)
            if (live.frame > it->second.frame + 8)
                it = live.seenRecent.erase(it);
            else
                live.seenNow.emplace(it->first, it->second.vertices), ++it;
        for (const auto& [id, sig] : g_liveIdSig)
            live.idSig[id] = sig;
        g_liveIdSig.clear();
        if (live.idSig.size() > 50000)
            live.idSig.clear();
        g_liveSeen.clear();
        live.statOverflow += g_liveOverflow, live.statOutOfBudget += g_liveOutOfBudget, live.statReserved += g_liveReserved;
        g_liveOverflow = g_liveOutOfBudget = 0, g_liveReserved = 0, g_liveProbeVerts = 0;
        if (!g_liveSunKeys.empty())
            g_livePrevSunKeys.swap(g_liveSunKeys);
        g_liveSunKeys.clear();
        g_liveSunMarked = 0;
        // (occlusion and bounce unticked in the menu: the sun is no longer useful, its map is only replayed by eighths)
        g_liveSunEighth = !live.sunShadows && (live.sunEighth || !(live.menuAo || live.menuGi));
    }
    // counters reset inside this frame's list (before any list written in the next window): those
    // of the slots (calibration) and those of the next window's part of the stream, after their last read
    auto zeroCounters = [&](D3D12_RESOURCE_STATES from) {
        Transition(list.Get(), g.counters.Get(), from, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(g.counters.Get(), 0, g.zeros.Get(), 0, kSlots * 8ull);
        if (zeroArea != ~0u)
            list->CopyBufferRegion(g.counters.Get(), (kSlots + uint64_t(zeroArea) * kLiveAreaCounters) * 8ull, g.zeros.Get(), 0, uint64_t(kLiveAreaCounters) * 8ull);
        Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    };
    // geometry budget: measurements arrived (three frames later), then choice of the RT scene's objects
    // for the next frames: by decreasing apparent size up to the budget (bonus to objects already
    // inside: no back and forth at the edge of the budget); unknown objects get in automatically (measured afterwards)
    {
        std::vector<uint32_t>& ids = live.statsIds[live.frame % 3];
        if (!ids.empty() && live.statsReadback[live.frame % 3])
        {
            const float* v = nullptr;
            if (SUCCEEDED(live.statsReadback[live.frame % 3]->Map(0, nullptr, (void**)&v)) && v)
            {
                for (size_t i = 0; i < ids.size(); i++)
                {
                    const float minZ = v[i * 4], size = v[i * 4 + 1];
                    const float importance = (minZ < 1e8f && size > 0) ? size / std::max(minZ, 0.3f) : 0.0f;
                    live.objects[ids[i]] = { importance, live.frame, uint32_t(v[i * 4 + 2]), minZ };
                    // ExecuteIndirect: a region filled to the end was truncated: maximum reserve on the next frame
                    if (live.idSig.count(ids[i]))
                    {
                        const std::vector<uint32_t>& caps = live.statsCaps[live.frame % 3];
                        const float written = v[i * 4 + 2], cap = i < caps.size() ? float(caps[i]) : 0.0f;
                        if (cap > 0 && written >= cap)
                            live.objects[ids[i]].vertices = 65536; // the next measurement will tell the true size
                    }
                }
                live.statsReadback[live.frame % 3]->Unmap(0, nullptr);
            }
            ids.clear();
            live.statsCaps[live.frame % 3].clear();
        }
        if (live.objects.size() > 200000)
            live.objects.clear();
        // budget per mesh (all its copies together): a copy's identity is only known at
        // execution, the choice to capture is made at writing. The copies' measurements combined: importance of the most
        // important, vertices written by the biggest, the oldest measurement
        struct MeshInfo
        {
            float importance = 0;
            uint32_t vertices = 0;
            uint64_t frame = ~0ull;
            float minZ = 1e9f;
            bool indirect = false;
        };
        std::unordered_map<uint32_t, MeshInfo> meshes;
        for (const auto& [id, o] : live.objects)
            if (const auto m = live.idMesh.find(id); m != live.idMesh.end())
            {
                MeshInfo& mi = meshes[m->second];
                mi.importance = std::max(mi.importance, o.importance);
                mi.vertices = std::max(mi.vertices, o.vertices);
                mi.frame = std::min(mi.frame, o.frame);
                mi.minZ = std::min(mi.minZ, o.minZ);
                mi.indirect = mi.indirect || live.idSig.count(id) != 0;
            }
        std::unordered_set<uint32_t> known, admit, probe;
        if (live.vertexBudget > 0)
        {
            std::vector<std::pair<float, uint32_t>> ranked;
            uint64_t used = 0;
            for (const auto& [mesh, vertices] : live.seenNow)
            {
                const auto it = meshes.find(mesh);
                if (it == meshes.end())
                {
                    used += vertices; // unknown: gets in automatically
                    continue;
                }
                known.insert(mesh);
                ranked.push_back({ it->second.importance * (live.admitted.count(mesh) ? 2.0f : 1.0f), mesh });
            }
            // camera-motion witnesses: drawn only once (otherwise their order from one frame to the next is not
            // reliable), direct draw, small, further than 4 m (not the character the camera follows); spread in distance: the
            // distant ones tell the rotation, the near ones the displacement; kept as long as they stay on screen
            if (live.geoOn && live.cameraOn && live.camSerialNow && !live.motionLog && !live.camLost)
                live.sentinels.clear(); // the pose comes from the game's camera
            else if (live.geoOn)
            {
                auto single = [&](uint32_t mesh, const MeshInfo& mi) { return live.seenNow[mesh] <= mi.vertices + mi.vertices / 20 + 3; };
                for (auto it = live.sentinels.begin(); it != live.sentinels.end();)
                {
                    const auto mi = meshes.find(*it);
                    if (live.seenNow.count(*it) && mi != meshes.end() && single(*it, mi->second))
                        ++it;
                    else
                        it = live.sentinels.erase(it);
                }
                if (live.sentinels.size() < kLiveSentinels)
                {
                    std::vector<std::pair<float, uint32_t>> candidates;
                    for (const auto& [score, mesh] : ranked)
                    {
                        const MeshInfo& mi = meshes[mesh];
                        const uint32_t reserved = live.seenNow[mesh];
                        if (!live.sentinels.count(mesh) && !mi.indirect && live.stableMeshes.count(mesh) && reserved >= 30 && reserved <= 3000 && single(mesh, mi) &&
                            mi.minZ >= 4.0f && mi.minZ <= 200.0f)
                            candidates.push_back({ mi.minZ, mesh });
                    }
                    std::sort(candidates.begin(), candidates.end());
                    const size_t room = kLiveSentinels - live.sentinels.size();
                    for (size_t k = 0; k < room && k < candidates.size(); k++)
                        live.sentinels.insert(candidates[candidates.size() <= room ? k : k * candidates.size() / room].second);
                }
                for (uint32_t mesh : live.sentinels)
                    used += live.seenNow[mesh], admit.insert(mesh);
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<std::pair<uint64_t, uint32_t>> stale;
            // a mesh too big for the remaining room is skipped, the next ones may get in; vertices counted:
            // those reserved by all its copies in this frame.
            // Stable scene: the number of game draws varies from one frame to the next (with the sub-pixel jitter, even when
            // paused), and at the edge of the budget meshes kept entering and leaving (6 to 7 per frame, still camera).
            // A mesh already inside keeps its place as long as it fits in the budget + 20 %; a new one only gets in within the
            // budget, two at most per frame, and not within two seconds of leaving
            uint32_t entries = 0;
            for (const auto& [score, mesh] : ranked)
            {
                if (admit.count(mesh))
                    continue; // witness
                const uint32_t vertices = live.seenNow[mesh];
                const bool inside = live.admitted.count(mesh) != 0;
                bool fits = used + vertices <= (inside ? live.vertexBudget + live.vertexBudget / 5 : live.vertexBudget);
                if (fits && !inside && live.geoOn)
                {
                    const auto left = live.meshLeft.find(mesh);
                    fits = entries < 2 && (left == live.meshLeft.end() || live.frame > left->second + 120);
                    entries += fits ? 1 : 0;
                }
                if (fits)
                    used += vertices, admit.insert(mesh);
                else
                    stale.push_back({ live.geoOn ? live.meshProbed[mesh] : meshes[mesh].frame, mesh });
            }
            // re-measurements (and geometry cache): in turn, the oldest first; huge mesh (grass,
            // foliage, thousands of draws): a share of its draws on each round
            std::sort(stale.begin(), stale.end());
            uint64_t probed = 0;
            std::unordered_map<uint32_t, uint32_t> strides, phases;
            for (const auto& [frame, mesh] : stale)
            {
                const uint32_t vertices = live.seenNow[mesh];
                const uint32_t stride = live.geoOn ? std::max(1u, (vertices + kLiveGeoMeshVerts - 1) / kLiveGeoMeshVerts) : 1u;
                if (probed + vertices / stride > live.probeBudget)
                    continue;
                probed += vertices / stride, probe.insert(mesh);
                // (a huge mesh is captured in parts: each in turn, and each keeps its objects in the cache)
                if (stride > 1)
                    strides[mesh] = stride, phases[mesh] = live.meshPhase[mesh]++ % stride;
                if (live.geoOn)
                    live.meshProbed[mesh] = live.frame;
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_liveProbeStride.swap(strides);
                g_liveProbePhase.swap(phases);
                g_liveProbeDraws.clear();
            }
            if (live.meshProbed.size() > 100000)
                live.meshProbed.clear();
            if (live.meshLeft.size() > 100000)
                live.meshLeft.clear();
            if (live.meshPhase.size() > 100000)
                live.meshPhase.clear();
        }
        if (live.frame % 300 == 150)
        {
            uint64_t unknownVerts = 0, knownVerts = 0;
            for (const auto& [mesh, vertices] : live.seenNow)
                (known.count(mesh) ? knownVerts : unknownVerts) += vertices;
            Log("URTI live: budget: %zu meshes on screen (%llu vertices measured, %llu not measured yet), %zu in the RT scene, %zu copies measured",
                live.seenNow.size(), (unsigned long long)knownVerts, (unsigned long long)unknownVerts, admit.size(), live.objects.size());
            // the biggest objects: reserved vertices (index count × instances) and really written
            std::vector<std::pair<uint32_t, uint32_t>> big;
            for (const auto& [id, vertices] : live.seenNow)
                big.push_back({ vertices, id });
            std::sort(big.begin(), big.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            uint64_t admittedReserved = 0;
            for (uint32_t mesh : admit)
                admittedReserved += live.seenNow[mesh];
            char top[256] = "";
            for (size_t i = 0; i < big.size() && i < 5; i++)
            {
                const auto it = meshes.find(big[i].second);
                char item[48];
                std::snprintf(item, sizeof(item), " %u/%u", big[i].first, it != meshes.end() ? it->second.vertices : 0u);
                std::strncat(top, item, sizeof(top) - std::strlen(top) - 1);
            }
            Log("URTI live: budget: reserved vertices of admitted meshes %llu; biggest meshes (reserved, all copies / biggest copy written):%s",
                (unsigned long long)admittedReserved, top);
        }
        live.churnIn = live.churnOut = 0;
        for (const uint32_t id : admit)
            live.churnIn += live.admitted.count(id) ? 0 : 1;
        for (const uint32_t id : live.admitted)
            if (!admit.count(id))
            {
                live.churnOut++;
                // left the frame's scene: captured for the cache from the next frame (its places keep it until then)
                live.meshLeft[id] = live.frame;
                if (live.geoOn)
                    live.meshProbed[id] = 0;
            }
        live.statChurn += live.churnIn + live.churnOut;
        live.admitted = admit;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveKnown.swap(known), g_liveAdmit.swap(admit), g_liveProbeWant.swap(probe);
        g_liveBudget = live.vertexBudget > 0;
        // (geometry cache: the per-frame limit was respected when choosing; here, only a hard limit, the draws
        // of a huge mesh do not all have the same size)
        g_liveBudgetVertices = live.vertexBudget, g_liveProbeBudget = live.geoOn ? std::max<uint64_t>(live.probeBudget, live.geoProbe) : live.probeBudget;
        g_liveSlotLimit = live.slotLimit;
        g_liveMainDepth = slot >= 0 ? g.slotDepth[slot] : 0;
        g_liveMainSlotIndex = slot;
        // learned ExecuteIndirect sizes: per command signature (every frame), and per identity
        if (live.frame % 10 == 0)
        {
            g_liveLearned.clear();
            for (const auto& [id, o] : live.objects)
                if (live.frame - o.frame < 120) // 0 included: an ExecuteIndirect that draws nothing reserves almost nothing
                    if (const auto m = live.idMesh.find(id); m != live.idMesh.end())
                    {
                        const auto [jt, added] = g_liveLearned.emplace(m->second, o.vertices);
                        if (!added)
                            jt->second = std::max(jt->second, o.vertices);
                    }
        }
    }
    // camera motion measured on our geometry, read three frames later
    if (const float* m = nullptr; live.motionReadback[live.frame % 3] && SUCCEEDED(live.motionReadback[live.frame % 3]->Map(0, nullptr, (void**)&m)) && m)
    {
        live.motFrames++;
        live.motPairs += uint64_t(m[14]);
        if (live.motionLog)
            Log("MOTION %llu valid %.0f pairs %.0f kept %.0f deviation %.4f t %.4f %.4f %.4f trace %.6f pose %.3f", (unsigned long long)live.frame, m[12], m[14], m[13], m[15], m[9], m[10], m[11],
                m[0] + m[4] + m[8], std::sqrt(m[41] * m[41] + m[42] * m[42] + m[43] * m[43]));
        if (m[12] > 0.5f)
        {
            const double tr = std::clamp((double(m[0]) + m[4] + m[8] - 1.0) * 0.5, -1.0, 1.0);
            const double rot = std::acos(tr) * 180.0 / 3.14159265, move = std::sqrt(double(m[9]) * m[9] + double(m[10]) * m[10] + double(m[11]) * m[11]);
            live.motValid++, live.motRot += rot, live.motTrans += move;
            if (move > 0.01) // (more than a centimetre per frame: the camera moves forward; the field-of-view measurement is skewed by it)
                live.motLastMoveFrame = live.frame;
            live.motKept += m[14] > 0 ? m[13] / m[14] : 0.0, live.motRms += m[15];
            if (!live.camLive)
                live.geoPoseDistance = std::sqrt(double(m[41]) * m[41] + double(m[42]) * m[42] + double(m[43]) * m[43]);
            // the camera moves (our measurement) and the game's vectors still cannot be read: they probably do not
            // contain the camera's motion (only the characters'); three seconds
            // like that and ours replace them
            if (live.motionSource == 0 && !live.mvSynthetic && live.mvConvention == 255 && (rot > 0.05 || move > 0.005) && ++live.motMovingNoGame >= 180)
            {
                live.mvSynthetic = true, live.mvConvention = 0;
                Log("URTI live: the camera moves and the game's motion vectors stay unreadable: camera motion measured on our "
                    "geometry instead");
                if (live.verified || live.fromStore)
                    LiveSaveCalibration();
            }
        }
        // enough pairs, and almost none accepts the slightest motion: camera cut (cutscene, fast
        // travel). A missed cut is not serious (the next round replaces everything, and rays discard what the
        // camera sees through); one clearing too many makes half the effect disappear for a second
        const bool badFit = !live.camLive && m[12] <= 0.5f && m[14] >= 256.0f && m[13] < 32.0f;
        live.motionReadback[live.frame % 3]->Unmap(0, nullptr);
        if (badFit)
        {
            live.statGeoBadFit++;
            if (live.frame > live.geoLastFlush + 120)
                LiveGeoFlush("camera motion impossible to measure");
        }
        if (live.motFrames >= 300)
        {
            const double v = std::max(1u, live.motValid);
            Log("URTI live: camera motion measured on our geometry: %u frames out of %u, rotation %.3f° and translation %.1f cm per frame on "
                "average, %.0f pairs received per frame, %.0f %% kept, deviation %.1f mm, %zu witnesses%s", live.motValid, live.motFrames, live.motRot / v, live.motTrans / v * 100.0,
                double(live.motPairs) / live.motFrames, live.motKept / v * 100.0, live.motRms / v * 1000.0, live.sentinels.size(), live.mvSynthetic ? " (motion vectors built from it)" : "");
            live.motFrames = live.motValid = 0, live.motRot = live.motTrans = live.motKept = live.motRms = 0, live.motPairs = 0;
        }
    }
    // sun-tracking summary, read three frames later
    if (const float* r = nullptr; live.sunReadback[live.frame % 3] && SUCCEEDED(live.sunReadback[live.frame % 3]->Map(0, nullptr, (void**)&r)) && r)
    {
        if ((live.gnMode & 3) == 2)
        {
            const float* o = r + 2048 / 4;
            const uint32_t* u = reinterpret_cast<const uint32_t*>(o);
            if (u[10] > 0)
                live.rotSum[0] += o[9], live.rotSum[1] += u[10], live.rotSum[2] += u[11] ? 1 : 0, live.rotSum[3] += 1;
            if (++live.rotLogFrames >= 300)
            {
                const double k = std::max(live.rotSum[3], 1.0);
                Log("URTI live: camera orientation from the G-buffer normals: %.0f frames out of 300 with pixels, mean agreement %.3f over %.0f pixels, valid %.0f %%; "
                    "lignes (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)",
                    live.rotSum[3], live.rotSum[0] / k, live.rotSum[1] / k, live.rotSum[2] / k * 100.0, o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8]);
                live.rotLogFrames = 0;
                for (double& v : live.rotSum)
                    v = 0;
            }
        }
        if (live.frame % 600 == 0)
            Log("URTI live: depth law of the frame: %g + %g / w (valid %.0f); stored: %g + %g / w", r[9], r[10], r[11], live.A, live.B);
        if (live.motionLog)
            Log("LOI %llu %g %g valide %.0f", (unsigned long long)live.frame, r[9], r[10], r[11]);
        if (live.calibrated)
        {
            live.sunFrames++, live.pairSum += uint64_t(r[4]);
            if (live.jitterLog) // urti.ini jitter_log=1: sub-pixel jitter measured every frame (the one from three frames ago)
                Log("JITTER %llu valid %.0f delta %.6f %.6f absolute %.6f %.6f s2 %.3f t2 %.3f", (unsigned long long)live.frame, r[156], r[157], r[158], r[296], r[297], r[304], r[305]);
            if (r[156] > 0) // camera rotation measured (byte 624)
            {
                const double tr = r[144] + r[149] + r[154]; // trace of the matrix (rows at 576, 592, 608)
                live.rotationSum += std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0)) * 180.0 / 3.14159265;
                live.rotationValid++;
            }
            if (r[7] > 0)
            {
                const float dot = r[0] * live.lastSun[0] + r[1] * live.lastSun[1] + r[2] * live.lastSun[2];
                if (live.framesSinceSun == 0 && dot > 0)
                {
                    const double deg = std::acos(std::min(1.0, double(dot))) * 180.0 / 3.14159265;
                    live.sunJitterSum += deg, live.sunJitterMax = std::max(live.sunJitterMax, deg), live.sunJitterCount++;
                }
                live.lastSun[0] = r[0], live.lastSun[1] = r[1], live.lastSun[2] = r[2];
                live.sunAccepted++, live.framesSinceSun = 0;
                if (!live.verified)
                {
                    live.verified = true;
                    // quiet check of the stored calibration. Not with the game's camera, which gives projection and depth law
                    // every frame: this full capture, at load, overwrites the stream while the game loads (GPU lost in one game)
                    live.forceCalibration = g_camCbvSize == 0;
                }
            }
            else
                live.framesSinceSun++;
            if (r[305] > 0)
                live.fovStretch = r[305]; // t²: (tan of the assumed half-field / tan of the true one)²
            // sun share in the game's image: median of each histogram (live_sunlight), smoothed in log space
            // (about half a second); too few pairs (no game shadow on screen): value kept
            const uint32_t* check = reinterpret_cast<const uint32_t*>(r) + 472; // octet 1888
            for (int k = 0; k < 6; k++)
                live.mapCheck[k] += check[k];
            for (int k = 0; k < 16; k++)
                live.depthHist[k] += check[8 + k], live.gapHist[k] += check[24 + k];
            // measurement accepted only if the map read agrees with the image: the points it lights are
            // clearly brighter than those it shadows (otherwise it is read wrongly: the previous value stays)
            const bool mapAgrees = check[0] >= 32 && check[1] >= 32 && check[5] > 0 &&
                                   (double(check[4]) / check[0]) > 1.3 * (double(check[5]) / check[1]);
            if (const uint32_t* u = reinterpret_cast<const uint32_t*>(r); live.sunMeasure && mapAgrees && u[308] >= 32)
            {
                float measured[4] = {};
                for (int c = 0; c < 4; c++)
                {
                    const uint32_t* h = u + 312 + c * 40;
                    uint32_t total = 0, below = 0;
                    for (int b = 0; b < 40; b++)
                        total += h[b];
                    int b = 0;
                    while (b < 39 && below + h[b] < total / 2)
                        below += h[b++];
                    const float f = h[b] ? std::clamp((total * 0.5f - below) / h[b], 0.0f, 1.0f) : 0.5f;
                    measured[c] = std::exp2((b + f) / 4.0f - 5.0f);
                }
                for (int c = 0; c < 4; c++)
                {
                    const float old = std::log2(live.sunSky[c]), now = std::log2(measured[c]);
                    live.sunSky[c] = std::exp2(live.sunSkyFrames ? old + (now - old) * 0.05f : now);
                }
                live.sunSkyFrames++, live.sunSkyPairs += u[308];
            }
            // the game's field of view: URTI's projection is corrected when the measurement is reliable (enough
            // camera rotation) and differs by more than 1.5 %; reflections then have exact angles (in
            // a stretched camera space, the floor's normal is wrong and the reflection shifted: it "floats")
            if (live.fovHold)
                live.fovHold--;
            // (rotation threshold 4e-4 -> 1e-4: with ordinary camera moves it was never reached; guard: the horizontal and
            // vertical stretches agree within 15 %, a consistent measurement)
            else if (live.fovAuto && live.fovForce <= 0 && !live.camLive && r[304] > 0 && r[305] > 0 && r[300] + r[302] > 1e-4f && std::fabs(r[304] / r[305] - 1.0f) < 0.15f)
            {
                const float k2 = (r[300] * r[304] + r[302] * r[305]) / (r[300] + r[302]);
                // stable over time: the same value (within 5 %) as at least 30 frames earlier; otherwise it becomes the reference. While walking, during
                // a burst, the measurement sometimes drifted on its own (37.9° -> 60.8° -> 91.2°)
                bool stable = false;
                if (live.fovK2Ref > 0 && live.frame >= live.fovK2RefFrame + 30)
                {
                    stable = std::fabs(k2 / live.fovK2Ref - 1.0f) < 0.05f;
                    live.fovK2Ref = stable ? 0.0f : k2, live.fovK2RefFrame = live.frame;
                }
                else if (live.fovK2Ref <= 0)
                    live.fovK2Ref = k2, live.fovK2RefFrame = live.frame;
                // (and the camera has not moved forward for 220 frames: the measurement's sums, which fade in ~200 frames, no longer contain any walking)
                if (stable && live.frame > live.motLastMoveFrame + 220 && std::fabs(k2 - 1.0f) > 0.03f)
                {
                    const float before = 2.0f * std::atan(1.0f / live.p11) * 57.2957795f;
                    const float aspect = live.p11 / live.p00;
                    // (no more than ~25 % at once, vertical field between 25° and 75°)
                    live.p11 = std::clamp(live.p11 * std::pow(std::clamp(k2, 0.6f, 1.7f), 0.4f), // 80 % of the difference: no oscillation
                                            1.0f / std::tan(37.5f / 57.2957795f), 1.0f / std::tan(12.5f / 57.2957795f));
                    live.p00 = live.p11 / aspect;
                    live.fovReset = true, live.fovHold = 6, live.fovMeasured = true;
                    LiveSaveCalibration();
                    Log("URTI live: field of view corrected: %.1f° -> %.1f°", before, 2.0f * std::atan(1.0f / live.p11) * 57.2957795f);
                }
            }
            if (live.sunFrames == 300)
            {
                if (live.fovStretch > 0 && live.p11 > 0 && !live.camLive)
                    Log("URTI live: the game's vertical field of view measured: %.1f° (URTI assumes %.1f°)",
                        2.0 * std::atan(1.0 / (live.p11 * std::sqrt(double(live.fovStretch)))) * 180.0 / 3.14159265,
                        2.0 * std::atan(1.0 / double(live.p11)) * 180.0 / 3.14159265);
                Log("URTI live: sun tracked (%.3f, %.3f, %.3f), measured on %.0f %% of frames, %.0f pairs per frame, %u cascades", r[0], r[1], r[2],
                    100.0 * live.sunAccepted / live.sunFrames, double(live.pairSum) / live.sunFrames, reinterpret_cast<const uint32_t*>(r)[8]);
                if (live.sunJitterCount)
                    Log("URTI live: sun from one frame to the next: %.3f° on average, %.2f° at most (camera included)", live.sunJitterSum / live.sunJitterCount,
                        live.sunJitterMax);
                live.sunJitterSum = live.sunJitterMax = 0, live.sunJitterCount = 0;
                if (live.rotationValid)
                    Log("URTI live: camera rotation measured on %u frames out of 300 (cascades), %.3f° per frame on average", live.rotationValid,
                        live.rotationSum / live.rotationValid);
                live.rotationValid = 0, live.rotationSum = 0;
                live.sunFrames = live.sunAccepted = live.pairSum = 0;
                if (live.sunSkyFrames > live.sunSkyLogged)
                    Log("URTI live: the game's light measured: sun / sky %.2f (red %.2f, green %.2f, blue %.2f); RT shadow facing the sun: "
                        "%.0f %% of the light removed; %u frames measured out of 300, %.0f pairs per frame", live.sunSky[3], live.sunSky[0], live.sunSky[1],
                        live.sunSky[2], 100.0f * live.sunSky[3] / (1.0f + live.sunSky[3]), live.sunSkyFrames - live.sunSkyLogged,
                        double(live.sunSkyPairs) / std::max(1u, live.sunSkyFrames - live.sunSkyLogged));
                else if (live.sunMeasure)
                    Log("URTI live: the game's light not measured (no game shadow on screen): sun / sky kept at %.2f", live.sunSky[3]);
                live.sunSkyLogged = live.sunSkyFrames, live.sunSkyPairs = 0;
                if (const uint64_t n = live.mapCheck[0] + live.mapCheck[1] + live.mapCheck[2] + live.mapCheck[3])
                {
                    Log("URTI live: the game's shadow map at the point seen (facing the sun): %.0f %% lit, %.0f %% shadowed, %.0f %% on the edge, "
                        "%.0f %% outside the cascades; mean luminance lit %.3f, shadowed %.3f (ratio %.2f: much more than 1 if the map read is the right one)",
                        100.0 * live.mapCheck[0] / n, 100.0 * live.mapCheck[1] / n, 100.0 * live.mapCheck[2] / n, 100.0 * live.mapCheck[3] / n,
                        live.mapCheck[0] ? live.mapCheck[4] / 1024.0 / live.mapCheck[0] : 0.0, live.mapCheck[1] ? live.mapCheck[5] / 1024.0 / live.mapCheck[1] : 0.0,
                        live.mapCheck[0] && live.mapCheck[1] && live.mapCheck[5] ? (double(live.mapCheck[4]) / live.mapCheck[0]) / (double(live.mapCheck[5]) / live.mapCheck[1]) : 0.0);
                    char depthText[160] = {}, gapText[160] = {};
                    for (int k = 0, a = 0, b = 0; k < 16; k++)
                    {
                        a += std::snprintf(depthText + a, sizeof(depthText) - a, "%s%llu", k ? " " : "", (unsigned long long)live.depthHist[k]);
                        b += std::snprintf(gapText + b, sizeof(gapText) - b, "%s%llu", k ? " " : "", (unsigned long long)live.gapHist[k]);
                    }
                    Log("URTI live: depth of the points in the cascade (-0.5 to 1.5 by 0.125): %s; map - point (-0.02 to 0.02 by 0.0025): %s",
                        depthText, gapText);
                }
                for (uint64_t& v : live.mapCheck)
                    v = 0;
                for (int k = 0; k < 16; k++)
                    live.depthHist[k] = live.gapHist[k] = 0;
            }
        }
        live.sunReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    if (live.mvConvention == 255 && live.mvErrorsReadback[live.frame % 3])
        if (const uint32_t* e = nullptr; SUCCEEDED(live.mvErrorsReadback[live.frame % 3]->Map(0, nullptr, (void**)&e)) && e)
        {
            const double meanMotion = e[33] ? e[32] / 1e6 / e[33] : 0;
            if (meanMotion > 0.0008) // the camera moves: the candidates can be told apart
            {
                for (int k = 0; k < 16; k++)
                    live.mvErr[k] += e[k * 2] / 10000.0, live.mvCount[k] += e[k * 2 + 1];
                if (++live.mvMovingFrames >= 90)
                {
                    int best = -1, second = -1;
                    double bestErr = 1e30, secondErr = 1e30;
                    for (int k = 0; k < 16; k++)
                    {
                        if (live.mvCount[k] < 500)
                            continue;
                        const double m = live.mvErr[k] / live.mvCount[k];
                        if (m < bestErr)
                            second = best, secondErr = bestErr, best = k, bestErr = m;
                        else if (m < secondErr)
                            second = k, secondErr = m;
                    }
                    if (best >= 0 && bestErr < 0.7 * secondErr)
                    {
                        live.mvConvention = uint32_t(best);
                        Log("URTI live: motion-vector convention found (%d: error %.4f, next %.4f): shadow accumulation on", best,
                            bestErr, secondErr);
                        if (live.verified || live.fromStore)
                            LiveSaveCalibration();
                    }
                    else if (++live.mvUndecided >= 3 && live.motionSource == 0 && ++live.mvGiveUps >= 2)
                    {
                        // two targets tried without a winner: the game's vectors are unreadable (seen in one
                        // game), those built from the camera motion measured on our geometry replace them
                        live.mvUndecided = 0, live.mvSynthetic = true, live.mvConvention = 0;
                        Log("URTI live: the game's motion vectors are unreadable: camera motion measured on our geometry instead");
                        if (live.verified || live.fromStore)
                            LiveSaveCalibration();
                    }
                    else if (live.mvUndecided >= 3)
                    {
                        // three measurements in motion without a winner: these are not motion vectors, the next target
                        // of the same format is tried
                        live.mvUndecided = 0, live.mvCandidate++;
                        Log("URTI live: motion vectors never told apart (best %.4f, next %.4f): trying target no. %u of this format",
                            bestErr, secondErr, live.mvCandidate);
                    }
                    else
                        Log("URTI live: motion vectors not told apart yet (best %.4f, next %.4f), going on", bestErr, secondErr);
                    std::fill(std::begin(live.mvErr), std::end(live.mvErr), 0.0), std::fill(std::begin(live.mvCount), std::end(live.mvCount), 0.0);
                    live.mvMovingFrames = 0;
                }
            }
            live.mvErrorsReadback[live.frame % 3]->Unmap(0, nullptr);
        }
    if (live.frameTrace)
    {
        Log("FRAME %llu t %.2f compute gen %d draws %u main %u regions %zu places %zu probes %zu cam %u", (unsigned long long)live.frame, LiveTraceMs(), int(live.imageGenerated),
            live.lastPresentDraws, drawsThisFrame, regionsNow.size(), placesNow.size(), probesNow.size(), live.camSerialNow);
        // row of depth and mask of this computation, read three computations later: did the computation see the whole frame?
        const uint32_t s = uint32_t(live.frame % 3);
        if (live.traceFrame[s] != ~0ull && live.traceRb[s])
            if (const uint32_t* v = nullptr; SUCCEEDED(live.traceRb[s]->Map(0, nullptr, (void**)&v)) && v)
            {
                const uint32_t n = live.traceWidth[s];
                uint32_t sky = 0;
                double sumD = 0, sumLit = 0;
                const bool d24 = live.depthFormat == DXGI_FORMAT_R24G8_TYPELESS || live.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
                for (uint32_t i = 0; i < n; i++)
                {
                    const float d = d24 ? float(v[i] & 0xFFFFFF) / 16777215.0f : reinterpret_cast<const float*>(v)[i];
                    sky += d == 0.0f || d == 1.0f ? 1 : 0;
                    sumD += d, sumLit += reinterpret_cast<const float*>(v)[n + i];
                }
                Log("ROW %llu sky %.1f depth %.6f mask %.4f", (unsigned long long)live.traceFrame[s], n ? 100.0 * sky / n : 0.0, n ? sumD / n : 0.0, n ? sumLit / n : 0.0);
                live.traceRb[s]->Unmap(0, nullptr);
                live.traceFrame[s] = ~0ull;
            }
    }
    const effect_texture_variable var = runtime->find_texture_variable("urti.fx", "UrtiDepth");
    resource_view depthSrv = {}, depthSrgb = {};
    if (var.handle)
        runtime->get_texture_binding(var, &depthSrv, &depthSrgb);
    ID3D12Resource* depthCopy = depthSrv.handle ? reinterpret_cast<ID3D12Resource*>(runtime->get_device()->get_resource_from_view(depthSrv).handle) : nullptr;
    const effect_uniform_variable active = runtime->find_uniform_variable("urti.fx", "UrtiActive");
    if (depthCopy && !live.depthReady)
    {
        // the depth law is measured every frame on the GPU: only the projection is assumed
        LiveSetProjection(depthCopy->GetDesc());
        live.depthReady = true;
    }
    if (slot < 0 || !depthCopy || !live.depthReady)
    {
        live.statNoDepth++;
        if (active.handle)
            runtime->set_uniform_value_bool(active, false);
        zeroCounters(D3D12_RESOURCE_STATE_STREAM_OUT);
        live.frame++;
        return;
    }
    const D3D12_RESOURCE_DESC depthDesc = depthCopy->GetDesc();
    if (live.fromStore && live.p00 == 0)
        LiveSetProjection(depthDesc);
    live.lastFilled = cursorNow / 16;
    if (!LiveEnsureCapacity(device.Get(), std::max<uint64_t>(std::max<uint64_t>(live.lastFilled, 300000), cursorNow / 16)) || !LiveEnsureImages(runtime, g.device, depthDesc) ||
        !LiveEnsureSun(g.device))
    {
        zeroCounters(D3D12_RESOURCE_STATE_STREAM_OUT);
        return;
    }

    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!live.timeHeap)
    {
        D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 24, 0 };
        g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&live.timeHeap));
        for (auto& r : live.timeReadback)
            r = MakeBuffer(g.device, 8 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC qdd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 3 * LiveState::kTimeD, 0 };
        g.device->CreateQueryHeap(&qdd, IID_PPV_ARGS(&live.timeDHeap));
        for (auto& r : live.timeDRb)
            r = MakeBuffer(g.device, LiveState::kTimeD * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        for (auto& r : live.timeCRb)
            r = MakeBuffer(g.device, LiveState::kTimeD * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    // detailed timing of the computation, read three frames later
    if (const uint64_t* t = nullptr; live.timeDRb[live.frame % 3] && SUCCEEDED(live.timeDRb[live.frame % 3]->Map(0, nullptr, (void**)&t)) && t)
    {
        UINT64 freq = 0;
        if (ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()); q && SUCCEEDED(q->GetTimestampFrequency(&freq)) && freq &&
            t[11] > t[0] && t[11] - t[0] < freq)
        {
            bool ok = true;
            for (int k = 0; k < 11; k++)
                ok = ok && t[k + 1] >= t[k];
            if (ok)
            {
                for (int k = 0; k < 11; k++)
                    live.timeDSum[k] += double(t[k + 1] - t[k]) * 1000.0 / double(freq);
                if (++live.timeDFrames == 300)
                {
                    const double n = live.timeDFrames;
                    Log("URTI live: GPU detail (ms per computation): vertices %.2f, motion %.2f, transparents %.2f, geometry cache %.2f, measurement %.2f, "
                        "sun and law %.2f, frame structure %.2f, scene structure %.2f, depth %.2f, shadows and occlusion %.2f, denoising %.2f",
                        live.timeDSum[0] / n, live.timeDSum[1] / n, live.timeDSum[2] / n, live.timeDSum[3] / n, live.timeDSum[4] / n, live.timeDSum[5] / n,
                        live.timeDSum[6] / n, live.timeDSum[7] / n, live.timeDSum[8] / n, live.timeDSum[9] / n, live.timeDSum[10] / n);
                    for (int k = 0; k < 11; k++)
                        live.timeDSum[k] = 0;
                    live.timeDFrames = 0;
                }
            }
        }
        live.timeDRb[live.frame % 3]->Unmap(0, nullptr);
    }
    // timing read three frames later: structure, sun, shadows, reflections, composition (ms)
    if (const uint64_t* t = nullptr; live.timeReadback[live.frame % 3] && SUCCEEDED(live.timeReadback[live.frame % 3]->Map(0, nullptr, (void**)&t)) && t)
    {
        UINT64 freq = 0;
        if (ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()); q && SUCCEEDED(q->GetTimestampFrequency(&freq)) && freq &&
            t[5] > t[0] && t[5] - t[0] < freq)
        {
            for (int k = 0; k < 5; k++)
                live.timeSum[k] += double(t[k + 1] - t[k]) * 1000.0 / double(freq);
            // time budget: the shadow pass (occlusion and denoising, markers 3 to 4: it used to be 2 to 3, the copy of the game's map, 0.02 ms: the regulator always believed
            // it had room) and the reflection pass, every frame. An overrun raises the level at once, the return is slow
            {
                const double aoNow = std::min(200.0, double(t[4] - t[3]) * 1000.0 / double(freq)), reflNow = std::min(200.0, double(t[5] - t[4]) * 1000.0 / double(freq));
                live.shadowMs = aoNow;
                live.statAoMax = std::max(live.statAoMax, aoNow), live.statReflMax = std::max(live.statReflMax, reflNow);
                live.statAoSpikes += aoNow > 12.0 ? 1u : 0u, live.statReflSpikes += reflNow > 8.0 ? 1u : 0u;
                // decision on the median of the last 9 frames: an isolated peak (often the game itself, taking the queue at the same moment: 25 to 50 ms
                // of "occlusion" recorded in the player's session) raised it by two levels, and quality came back only one level every 40
                // frames: permanently at the lowest, the noisiest image
                auto regulate = [](int& level, double now, double& ema, float target, uint32_t& cool, double* hist, uint32_t& n) {
                    ema += (now - ema) * 0.25;
                    hist[n++ % 9] = now;
                    if (cool)
                        cool--;
                    if (target <= 0)
                    {
                        level = 0;
                        return;
                    }
                    const uint32_t m = std::min<uint32_t>(n, 9);
                    double s[9];
                    std::copy(hist, hist + m, s);
                    std::nth_element(s, s + m / 2, s + m);
                    const double med = s[m / 2];
                    if (cool == 0 && med > target * 1.25)
                        level = std::min(4, level + 1), cool = 6; // (the result is read three frames later)
                    else if (cool == 0 && level > 0 && med < target * 0.65)
                        level--, cool = 30;
                };
                regulate(live.aoLevel, aoNow, live.aoEma, live.aoTarget, live.aoCool, live.aoHist, live.aoHistN);
                regulate(live.reflLevel, reflNow, live.reflEma, live.reflTarget, live.reflCool, live.reflHist9, live.reflHistN);
                // (the reach of reflections never shrinks: without the far trees the water turned bright green; and occlusion is never limited to the very near)
                static const uint32_t kAoRays[5] = { 4, 3, 2, 2, 2 }, kAoHops[5] = { 2, 2, 1, 1, 0 }, kRStride[5] = { 2, 2, 2, 2, 4 }, kRHops[5] = { 3, 3, 2, 1, 0 };
                // (reflections: one pixel in two at any level, the other keeps its history; with the world cache, a shorter ray ends on the sky
                // and distance map, no longer on the game's water: the range may drop. Occlusion: never a single ray (noise), the range drops)
                static const float kAoDist[5] = { 50.0f, 40.0f, 28.0f, 18.0f, 12.0f }, kRMaxRad[5] = { 500.0f, 300.0f, 200.0f, 140.0f, 90.0f };
                static const float kRMax[5] = { 500.0f, 500.0f, 500.0f, 500.0f, 500.0f };
                live.aoRays = kAoRays[live.aoLevel], live.aoHopsNow = kAoHops[live.aoLevel], live.aoDistNow = kAoDist[live.aoLevel];
                live.reflStrideNow = kRStride[live.reflLevel], live.reflHopsNow = kRHops[live.reflLevel], live.reflTMaxNow = live.reflMixValid ? kRMaxRad[live.reflLevel] : kRMax[live.reflLevel];
                if (live.reflFull && live.reflLevel == 0)
                    live.reflStrideNow = 1;
                live.statAoLevelSum += uint32_t(live.aoLevel), live.statReflLevelSum += uint32_t(live.reflLevel);
            }
            if (const uint32_t s = uint32_t(live.frame % 3); live.slotBuilt[s])
            {
                const double build = std::min(50.0, double(t[2] - t[1]) * 1000.0 / double(freq)), trace = std::min(100.0, double(t[5] - t[3]) * 1000.0 / double(freq));
                if (!live.slotRefit[s])
                {
                    live.rebuildMs += (build - live.rebuildMs) * 0.2;
                    // geometry budget: every 30 frames, -15 % above the target (+10 %), +8 % well below
                    if (live.buildTarget > 0 && live.budgetMax > 0 && live.frame % 30 == 0)
                    {
                        const uint64_t kBudgetMin = live.geoOn ? 500000 : 1500000;
                        if (live.rebuildMs > live.buildTarget * 1.1)
                            live.vertexBudget = std::max<uint64_t>(std::min<uint64_t>(kBudgetMin, live.budgetMax), live.vertexBudget * 85 / 100);
                        else if (live.rebuildMs < live.buildTarget * 0.75)
                            live.vertexBudget = std::min<uint64_t>(live.budgetMax, live.vertexBudget * 108 / 100);
                    }
                    // the update cost slowly returns to a plausible value: a single aberrant
                    // measurement (busy frame, another program) does not block updates forever
                    live.refitMs += (0.3 * live.rebuildMs - live.refitMs) * 0.05;
                    if (live.slotCycle[s] == live.buildCycle)
                        live.traceBase = trace;
                }
                else
                {
                    live.refitMs += (build - live.refitMs) * 0.2;
                    if (live.slotCycle[s] == live.buildCycle && live.traceBase >= 0)
                        live.refitExtra += std::max(0.0, trace - live.traceBase);
                }
                live.slotBuilt[s] = false;
            }
            if (t[7] > t[6] && t[7] - t[6] < freq)
                live.timeSum[5] += double(t[7] - t[6]) * 1000.0 / double(freq);
            if (++live.timeFrames == 300)
            {
                const double n = live.timeFrames;
                Log("URTI live: time budget: occlusion mean level %.1f (worst frame %.1f ms, %u frames above 12 ms, target %.1f), reflections mean level %.1f (worst frame %.1f ms, %u frames above 8 ms, target %.1f)",
                    live.statAoLevelSum / n, live.statAoMax, live.statAoSpikes, live.aoTarget, live.statReflLevelSum / n, live.statReflMax, live.statReflSpikes, live.reflTarget);
                live.statAoMax = live.statReflMax = 0, live.statAoSpikes = live.statReflSpikes = live.statAoLevelSum = live.statReflLevelSum = 0;
                Log("URTI live: GPU per frame: structure %.2f ms, sun and depth %.2f, game map copy %.2f, shadows (%u rays) %.2f, "
                    "reflets %.2f, composition %.2f", live.timeSum[0] / n, live.timeSum[1] / n, live.timeSum[2] / n, live.shadowRays, live.timeSum[3] / n,
                    live.timeSum[4] / n, live.timeSum[5] / n);
                const uint32_t builds = live.statRebuilds + live.statUpdates;
                if (builds)
                    Log("URTI live: acceleration structure: %.0f vertices on average (capacity %llu), %u rebuilds and %u updates "
                        "(rebuild %.2f ms, update %.2f ms)", double(live.statVertices) / builds, (unsigned long long)live.capacity,
                        live.statRebuilds, live.statUpdates, live.rebuildMs, live.refitMs);
                if (live.statPlaceFrames)
                {
                    const double f = double(live.statPlaceFrames);
                    Log("URTI live: fixed places: %.0f held (%.0f MB), used part %.0f MB out of %.0f, %.1f new per frame, %llu redistributions",
                        live.statPlaces / f, live.statPlaceHeld / f / 1048576.0, live.statPlaceHigh / f / 1048576.0, kSlotBytes / 1048576.0,
                        live.statPlaceNew / f, (unsigned long long)live.statPlaceResets);
                }
                live.statPlaces = live.statPlaceHeld = live.statPlaceHigh = live.statPlaceNew = live.statPlaceFrames = live.statPlaceResets = 0;
                live.statRebuilds = live.statUpdates = 0, live.statVertices = 0;
                if (live.statBudgetFrames)
                {
                    const double f = double(live.statBudgetFrames);
                    Log("URTI live: geometry budget %llu vertices: %.0f draws in the RT scene (%.0f vertices reserved), %.0f out of budget, "
                        "%.0f re-measured, %.1f refused (slot full), %.1f entered or left the RT scene per frame", (unsigned long long)live.vertexBudget,
                        live.statDraws / f, live.statReserved / f, live.statOutOfBudget / f, live.statProbes / f, live.statOverflow / f, live.statChurn / f);
                }
                live.statChurn = 0;
                live.statDraws = live.statOutOfBudget = live.statProbes = live.statOverflow = live.statBudgetFrames = live.statReserved = 0;
                std::fill(std::begin(live.timeSum), std::end(live.timeSum), 0.0);
                live.timeFrames = 0;
            }
        }
        live.timeReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    LiveTime(list.Get(), 0);
    LiveTimeD(list.Get(), 0);
    // 1. screen vertices to camera space (NaN beyond what was filled in each region);
    // only the part of the stream assigned to this frame: the structure covers only that
    Transition(list.Get(), g.so.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, srv);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, srv);
    const std::vector<LiveRegion>& tableNow = persistNow ? placesNow : regionsNow; // converted regions (RT scene)
    ID3D12Resource* placesBuf = nullptr;
    if (persistNow)
    {
        ComPtr<ID3D12Resource>& up = live.placeUpload[live.frame % 3];
        const uint64_t bytes = std::max<size_t>(placesNow.size(), 1) * sizeof(LiveRegion);
        if (!up || up->GetDesc().Width < bytes)
            Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        if (up && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            if (!placesNow.empty())
                std::memcpy(p, placesNow.data(), placesNow.size() * sizeof(LiveRegion));
            else
                std::memset(p, 0, sizeof(LiveRegion));
            up->Unmap(0, nullptr);
            placesBuf = up.Get();
        }
        if (!placesBuf)
        {
            zeroCounters(srv), Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
            return;
        }
    }
    ID3D12Resource* regionsBuf = nullptr;
    {
        ComPtr<ID3D12Resource>& up = live.regionUpload[live.frame % 3];
        const uint64_t bytes = std::max<size_t>(regionsNow.size(), 1) * sizeof(LiveRegion);
        if (!up || up->GetDesc().Width < bytes)
            Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        if (up && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            if (!regionsNow.empty())
                std::memcpy(p, regionsNow.data(), regionsNow.size() * sizeof(LiveRegion));
            else
                std::memset(p, 0, sizeof(LiveRegion));
            up->Unmap(0, nullptr);
            regionsBuf = up.Get();
        }
    }
    if (!regionsBuf)
    {
        zeroCounters(srv), Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
        return;
    }
    // per region: identity, start of the same object's region in the previous frame (+ 1; 0: unknown)
    ID3D12Resource* linksBuf = nullptr;
    {
        live.pendingStart.clear();
        std::vector<uint32_t> links(std::max<size_t>(tableNow.size(), 1) * 2, 0);
        for (size_t i = 0; i < tableNow.size(); i++)
        {
            const LiveRegion& r = tableNow[i];
            links[i * 2] = r.id;
            if (!r.id)
                continue;
            if (live.prevValid)
                if (auto it = live.prevStart.find(r.id); it != live.prevStart.end())
                    links[i * 2 + 1] = it->second + 1;
            live.pendingStart.emplace(r.id, r.offset / 16);
        }
        ComPtr<ID3D12Resource>& up = live.linkUpload[live.frame % 3];
        const uint64_t bytes = links.size() * 4;
        if (!up || up->GetDesc().Width < bytes)
            Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        if (up && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            std::memcpy(p, links.data(), bytes);
            up->Unmap(0, nullptr);
            linksBuf = up.Get();
        }
    }
    // start in the stream of the region of each entry of the converted table; this frame's regions sorted by their
    // start in the stream, for the depth law (live_depthfit draws its vertices there)
    const std::vector<uint32_t>& srcTable = persistNow ? placeSrc : srcNow;
    std::vector<LiveRegion> fitNow(regionsNow.size());
    uint32_t fitEnd = 0;
    for (size_t i = 0; i < regionsNow.size(); i++)
        fitNow[i] = { srcNow[i], regionsNow[i].capacity, regionsNow[i].counter, regionsNow[i].id }, fitEnd = std::max(fitEnd, srcNow[i] + regionsNow[i].capacity);
    std::sort(fitNow.begin(), fitNow.end(), [](const LiveRegion& a, const LiveRegion& b) { return a.offset < b.offset; });
    auto upload = [&](ComPtr<ID3D12Resource>& up, const void* data, uint64_t bytes) -> ID3D12Resource* {
        bytes = std::max<uint64_t>(bytes, 16);
        if (!up || up->GetDesc().Width < bytes)
            Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        if (!up || FAILED(up->Map(0, nullptr, &p)))
            return nullptr;
        std::memset(p, 0, 16);
        if (data)
            std::memcpy(p, data, bytes);
        up->Unmap(0, nullptr);
        return up.Get();
    };
    ID3D12Resource* srcBuf = upload(live.srcUpload[live.frame % 3], srcTable.empty() ? nullptr : srcTable.data(), srcTable.size() * 4);
    ID3D12Resource* fitBuf = upload(live.fitUpload[live.frame % 3], fitNow.empty() ? nullptr : fitNow.data(), fitNow.size() * sizeof(LiveRegion));
    if (!linksBuf || !srcBuf || !fitBuf)
    {
        zeroCounters(srv), Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
        return;
    }
    // the RT scene's region table copied into video memory: each vertex (conversion), each reflection and the surface
    // cache look up their region there; in the upload buffer, every read crossed the PCIe bus
    ID3D12Resource* sceneTable = persistNow ? placesBuf : regionsBuf;
    {
        const uint64_t bytes = std::max<size_t>(tableNow.size(), 1) * sizeof(LiveRegion);
        if (!live.sceneGpu || live.sceneGpu->GetDesc().Width < bytes)
        {
            Retire(live.sceneGpu);
            live.sceneGpu = UavBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_RESOURCE_STATE_COPY_DEST);
            live.sceneGpuReadable = false;
        }
        if (live.sceneGpu)
        {
            if (live.sceneGpuReadable)
                Transition(list.Get(), live.sceneGpu.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.sceneGpu.Get(), 0, sceneTable, 0, bytes);
            Transition(list.Get(), live.sceneGpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            live.sceneGpuReadable = true;
            sceneTable = live.sceneGpu.Get();
        }
    }
    list->SetComputeRootSignature(live.convertRs.Get());
    list->SetPipelineState(live.convertPso.Get());
    list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
    list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, sceneTable->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, srcBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, live.view->GetGPUVirtualAddress());
    if (!live.pairsBuf)
    {
        live.pairsBuf = UavBuffer(g.device, 16 + uint64_t(kLiveMaxMotionPairs) * 24, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.motionBuf = UavBuffer(g.device, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto& r : live.motionReadback)
            r = MakeBuffer(g.device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    list->SetComputeRootUnorderedAccessView(5, live.pairsBuf ? live.pairsBuf->GetGPUVirtualAddress() : live.view->GetGPUVirtualAddress());
    const uint32_t converted = std::max(3u, uint32_t(std::min<uint64_t>(cursorNow / 16, live.capacity)) / 3 * 3);
    // geometry cache: the pairs come from the witnesses' reference points (live_refpairs), not from one frame to the next
    const bool refPose = live.geoOn && live.refpairsPso;
    // the game's camera: pose, motion, projection, depth law of this frame, exact
    float camData[48] = {};
    const bool trueCam = LiveCameraFrame(camData);
    // motion_log=1: the witness measurement goes on, separately, to check the camera read
    const bool camVerify = trueCam && live.motionLog && refPose;
    const float shearPrev[2] = { live.camShear[0], live.camShear[1] };
    if (trueCam)
        live.camShear[0] = camData[44], live.camShear[1] = camData[45];
    // places kept without being drawn: their vertices follow the camera (otherwise, in motion, they stayed where the camera
    // saw them at their last draw: up to three frames of motion off)
    const struct { float p00, p11; uint32_t count, regionCount, maxPairs, heldStep, pad1, pad2; float step[12]; float shearA[2], shearB[2]; } cc = {
        live.p00, live.p11, converted, uint32_t(tableNow.size()), live.pairsBuf && !refPose && !trueCam ? kLiveMaxMotionPairs : 0u,
        trueCam && camData[9] * camData[9] + camData[10] * camData[10] + camData[11] * camData[11] < 900.0f ? 1u : 0u, 0, 0, // (camera cut: not moved)
        { camData[0], camData[1], camData[2], camData[9], camData[3], camData[4], camData[5], camData[10], camData[6], camData[7], camData[8], camData[11] },
        { shearPrev[0], shearPrev[1] }, { live.camShear[0], live.camShear[1] } };
    list->SetComputeRoot32BitConstants(6, 24, &cc, 0);
    list->Dispatch(UINT((converted + 255) / 256), 1, 1);
    LiveTimeD(list.Get(), 1);
    // 1d. camera motion since the previous frame, measured on the conversion's vertex pairs
    if (live.camfitPso && live.pairsBuf && live.motionBuf && live.motionReadback[live.frame % 3])
    {
        if (trueCam)
            if (ID3D12Resource* up = upload(live.camUpload[live.frame % 3], camData, sizeof(camData)))
            {
                Transition(list.Get(), live.motionBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyBufferRegion(live.motionBuf.Get(), 0, up, 0, sizeof(camData));
                Transition(list.Get(), live.motionBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        if (camVerify && !live.motionCheck)
            live.motionCheck = UavBuffer(g.device, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (refPose && (!trueCam || camVerify))
        {
            if (!live.sentRefs)
            {
                live.sentRefs = UavBuffer(g.device, 208ull * (kLiveSentinels * 2), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                live.sentinelFree.clear();
            }
            // places of the points: those of departed witnesses become free again
            for (auto it = live.sentinelRef.begin(); it != live.sentinelRef.end();)
                if (live.sentinels.count(it->first))
                    ++it;
                else
                    live.sentinelFree.push_back(it->second), it = live.sentinelRef.erase(it);
            if (live.sentinelRef.empty() && live.sentinelFree.size() != kLiveSentinels * 2)
            {
                live.sentinelFree.clear();
                for (uint32_t k = 0; k < kLiveSentinels * 2; k++)
                    live.sentinelFree.push_back(k);
            }
            std::vector<LiveRegion> sentTable;
            std::unordered_set<uint32_t> sentSeen;
            for (size_t i = 0; i < regionsNow.size(); i++)
            {
                const auto m = live.idMesh.find(regionsNow[i].id);
                if (m == live.idMesh.end() || !live.sentinels.count(m->second) || !sentSeen.insert(m->second).second)
                    continue;
                uint32_t place = 0;
                if (const auto it = live.sentinelRef.find(m->second); it != live.sentinelRef.end())
                    place = it->second;
                else if (!live.sentinelFree.empty())
                {
                    place = live.sentinelFree.back() | 0x80000000u;
                    live.sentinelFree.pop_back();
                    live.sentinelRef[m->second] = place & 0x7FFFFFFF;
                }
                else
                    continue;
                sentTable.push_back({ srcNow[i], regionsNow[i].capacity, regionsNow[i].counter, place });
            }
            ID3D12Resource* sentBuf = sentTable.empty() ? nullptr : upload(live.sentUpload[live.frame % 3], sentTable.data(), sentTable.size() * sizeof(LiveRegion));
            if (live.sentRefs && sentBuf)
            {
                list->SetComputeRootSignature(live.refpairsRs.Get());
                list->SetPipelineState(live.refpairsPso.Get());
                list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
                list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(2, sentBuf->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(3, live.sentRefs->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(4, live.pairsBuf->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(5, live.motionBuf->GetGPUVirtualAddress());
                const struct { float p00, p11; uint32_t count, maxPairs, identityPose, pad0, pad1, pad2; } rp = {
                    live.p00, live.p11, uint32_t(sentTable.size()), kLiveMaxMotionPairs, live.geoResetPose ? 1u : 0u, 0, 0, 0 };
                list->SetComputeRoot32BitConstants(6, 8, &rp, 0);
                list->Dispatch(UINT(sentTable.size()), 1, 1);
            }
        }
        ID3D12Resource* motionOut = camVerify && live.motionCheck ? live.motionCheck.Get() : live.motionBuf.Get();
        if (!trueCam || (camVerify && live.motionCheck))
        {
            D3D12_RESOURCE_BARRIER pb = {};
            pb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            pb.UAV.pResource = live.pairsBuf.Get();
            list->ResourceBarrier(1, &pb);
            list->SetComputeRootSignature(live.camfitRs.Get());
            list->SetPipelineState(live.camfitPso.Get());
            list->SetComputeRootUnorderedAccessView(0, motionOut->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(1, live.pairsBuf->GetGPUVirtualAddress());
            const uint32_t fc[4] = { kLiveMaxMotionPairs, live.geoResetPose ? 1u : 0u, 0, 0 };
            live.geoResetPose = false;
            list->SetComputeRoot32BitConstants(2, 4, fc, 0);
            list->Dispatch(1, 1, 1);
        }
        Transition(list.Get(), motionOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.motionReadback[live.frame % 3].Get(), 0, motionOut, 0, 176);
        Transition(list.Get(), motionOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    LiveTimeD(list.Get(), 2);
    // 1e. transparent surfaces (water, glass): their layer converted, then only the flat surfaces kept
    // (window without any, game frame split between two presents: the last layer stays, the water's reflection
    // does not go out every other frame)
    if (!live.waterOn || (transpNow.empty() && ++live.transpMiss > 8))
        live.transpBuilt = false;
    if (live.waterOn && !transpNow.empty() && live.planesPso)
    {
        live.transpMiss = 0;
        if (!live.viewT)
        {
            live.viewT = UavBuffer(g.device, uint64_t(kLiveTranspVertices) * 12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.planeStats = UavBuffer(g.device, 16 + 256 * 24, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (auto& r : live.planeReadback)
                r = MakeBuffer(g.device, 16 + 256 * 24, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RAYTRACING_GEOMETRY_DESC tg = {};
            tg.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            tg.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            tg.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            tg.Triangles.VertexBuffer.StrideInBytes = 12;
            tg.Triangles.VertexCount = kLiveTranspVertices;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti = {};
            ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            ti.NumDescs = 1;
            ti.pGeometryDescs = &tg;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
            device->GetRaytracingAccelerationStructurePrebuildInfo(&ti, &pi);
            live.blasT = UavBuffer(g.device, pi.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            live.blasTScratch = UavBuffer(g.device, pi.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.viewTRead = false;
            Log("URTI live: transparent-surface layer for %u vertices (%.1f MB)", kLiveTranspVertices, pi.ResultDataMaxSizeInBytes / 1048576.0);
        }
        auto uploadT = [&](ComPtr<ID3D12Resource>& up, const void* data, uint64_t bytes) -> ID3D12Resource* {
            if (!up || up->GetDesc().Width < bytes)
                Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p = nullptr;
            if (!up || FAILED(up->Map(0, nullptr, &p)))
                return nullptr;
            std::memcpy(p, data, bytes);
            up->Unmap(0, nullptr);
            return up.Get();
        };
        ID3D12Resource* tTable = uploadT(live.transpUpload[live.frame % 3], transpNow.data(), transpNow.size() * sizeof(LiveRegion));
        ID3D12Resource* tSrc = uploadT(live.transpSrcUpload[live.frame % 3], transpSrcNow.data(), transpSrcNow.size() * 4);
        const uint32_t tCount = std::max(3u, std::min(transpCursorNow / 16, kLiveTranspVertices) / 3 * 3);
        if (live.motionLog)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Log("TLAYER %llu draws %zu vertices %u refused %u", (unsigned long long)live.frame, transpNow.size(), tCount, g_liveTranspRefused);
            g_liveTranspRefused = 0;
        }
        if (live.viewT && live.blasT && live.blasTScratch && tTable && tSrc && live.planeStats && live.planeReadback[live.frame % 3])
        {
            if (live.viewTRead)
                Transition(list.Get(), live.viewT.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), live.viewTRead = false;
            list->SetComputeRootSignature(live.convertRs.Get());
            list->SetPipelineState(live.convertPso.Get());
            list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress()); // starts across the whole stream
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, tTable->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, tSrc->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, live.viewT->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(5, live.pairsBuf ? live.pairsBuf->GetGPUVirtualAddress() : live.viewT->GetGPUVirtualAddress());
            const struct { float p00, p11; uint32_t count, regionCount, maxPairs, pad0, pad1, pad2; } tc = {
                live.p00, live.p11, tCount, uint32_t(transpNow.size()), 0, 0, 0, 0 };
            list->SetComputeRoot32BitConstants(6, 8, &tc, 0);
            list->Dispatch(UINT((tCount + 255) / 256), 1, 1);
            D3D12_RESOURCE_BARRIER vb = {};
            vb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            vb.UAV.pResource = live.viewT.Get();
            list->ResourceBarrier(1, &vb);
            // sorting: flat surfaces kept, the rest invisible to rays; summary read three frames later
            Transition(list.Get(), live.planeStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.planeStats.Get(), 0, g.zeros.Get(), 0, 16);
            Transition(list.Get(), live.planeStats.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetComputeRootSignature(live.planesRs.Get());
            list->SetPipelineState(live.planesPso.Get());
            list->SetComputeRootShaderResourceView(0, tTable->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2, live.viewT->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.planeStats->GetGPUVirtualAddress());
            const uint32_t pc[4] = { uint32_t(transpNow.size()), 0, 0, 0 };
            list->SetComputeRoot32BitConstants(4, 4, pc, 0);
            list->Dispatch(UINT(transpNow.size()), 1, 1);
            Transition(list.Get(), live.planeStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.planeReadback[live.frame % 3].Get(), 0, live.planeStats.Get(), 0, 16 + 256 * 24);
            Transition(list.Get(), live.planeStats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list.Get(), live.viewT.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv), live.viewTRead = true;
            // its structure, rebuilt every frame (a few surfaces)
            D3D12_RAYTRACING_GEOMETRY_DESC tg = {};
            tg.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            tg.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            tg.Triangles.VertexBuffer = { live.viewT->GetGPUVirtualAddress(), 12 };
            tg.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            tg.Triangles.VertexCount = tCount;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tb = {};
            tb.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            tb.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            tb.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            tb.Inputs.NumDescs = 1;
            tb.Inputs.pGeometryDescs = &tg;
            tb.DestAccelerationStructureData = live.blasT->GetGPUVirtualAddress();
            tb.ScratchAccelerationStructureData = live.blasTScratch->GetGPUVirtualAddress();
            list->BuildRaytracingAccelerationStructure(&tb, 0, nullptr);
            vb.UAV.pResource = live.blasT.Get();
            list->ResourceBarrier(1, &vb);
            live.transpBuilt = true;
        }
    }
    // transparent-surface summary, read three frames later
    if (const uint32_t* c = nullptr; live.planeReadback[live.frame % 3] && SUCCEEDED(live.planeReadback[live.frame % 3]->Map(0, nullptr, (void**)&c)) && c)
    {
        live.planeFrames++, live.planeKept += c[0], live.planeTris += c[1], live.planeSeen += c[2];
        if (live.motionLog)
        {
            char text[2400] = "";
            // roughly horizontal and large enough surfaces, those that may be water
            for (uint32_t k = 0; k < c[2] && k < 256; k++)
                if (const uint32_t v = c[4 + k * 6]; (v & 0x1FFFFFFF) >= 30 && (c[5 + k * 6] >> 16) >= 300 && std::fabs(reinterpret_cast<const float*>(c)[7 + k * 6]) > 0.6f)
                {
                    char item[64];
                    float nd[4];
                    std::memcpy(nd, c + 6 + k * 6, sizeof(nd));
                    std::snprintf(item, sizeof(item), " %u%s/%u/%u/n%.2f,%.2f,%.2f/d%.2f", v & 0x1FFFFFFF, (v >> 31) ? "P" : (v >> 29) & 1 ? "F" : (v >> 30) & 1 ? "c" : "x", c[5 + k * 6] & 0xFFFF, c[5 + k * 6] >> 16, nd[0], nd[1], nd[2], nd[3]);
                    std::strncat(text, item, sizeof(text) - std::strlen(text) - 1);
                }
            Log("LAYER %llu received %u planes %u :%s", (unsigned long long)live.frame, c[2], c[0], text);
        }
        live.planeReadback[live.frame % 3]->Unmap(0, nullptr);
        if (live.planeFrames >= 300)
        {
            Log("URTI live: transparent surfaces: %.1f draws per frame, of which %.1f flat ones kept (water, glass: %.0f triangles)",
                double(live.planeSeen) / live.planeFrames, double(live.planeKept) / live.planeFrames, double(live.planeTris) / live.planeFrames);
            live.planeFrames = live.planeKept = live.planeTris = live.planeSeen = 0;
        }
    }
    LiveTimeD(list.Get(), 3);
    // 1f. geometry cache: objects captured in turn (measurement area) are converted into the cache's frame,
    // each one gets its structure, kept until the next round (a whole forest: 14 million vertices, which no
    // per-frame rebuild can follow). Built first from the objects of the frame's scene, removed from the cache
    if (live.geoOn && !live.geoPool && live.geoconvPso && live.geoinstPso && live.motionBuf)
    {
        live.geoPoolBytes = live.geoWantBytes;
        live.geoPool = UavBuffer(g.device, live.geoPoolBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        live.geoView = UavBuffer(g.device, uint64_t(kLiveGeoFrameVerts) * 12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.geoScratch = UavBuffer(g.device, kLiveGeoScratchBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.geoInstances = UavBuffer(g.device, (uint64_t(kLiveGeoMaxInstances) + 2) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        live.geoColors = UavBuffer(g.device, uint64_t(kLiveGeoMaxInstances) * LiveState::kGeoColorBins * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!live.geoPool || !live.geoView || !live.geoScratch || !live.geoInstances)
            live.geoOn = false, Log("URTI live: geometry cache: memory refused (%llu MB), off", (unsigned long long)(live.geoPoolBytes / 1048576));
        else
            Log("URTI live: geometry cache: reserve of %llu MB", (unsigned long long)(live.geoPoolBytes / 1048576));
    }
    const bool geoActive = live.geoOn && live.geoPool && live.geoView && live.geoScratch && live.geoInstances;
    if (geoActive)
    {
        auto geoErase = [&](uint32_t id) {
            if (const auto it = live.geo.find(id); it != live.geo.end())
            {
                live.geoByOffset.erase(it->second.offset);
                live.geo.erase(it);
            }
        };
        for (const LiveRegion& r : regionsNow)
            geoErase(r.id);
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms;
        std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC> builds;
        std::vector<LiveRegion> geoRegions;
        geoms.reserve(kLiveGeoMaxBuilds), builds.reserve(kLiveGeoMaxBuilds), geoRegions.reserve(kLiveGeoMaxBuilds);
        const D3D12_GPU_VIRTUAL_ADDRESS viewVa = live.geoView->GetGPUVirtualAddress(), poolVa = live.geoPool->GetGPUVirtualAddress(),
                                        scratchVa = live.geoScratch->GetGPUVirtualAddress();
        uint32_t frameVerts = 0, newVerts = 0, refreshVerts = 0;
        uint64_t scratchUsed = 0;
        if (live.geoSlotOwner.empty())
            live.geoSlotOwner.assign(kLiveGeoMaxInstances, 0u), live.geoSlotGen.assign(kLiveGeoMaxInstances, 0);
        // place in the colour table: the object's own if it already had one (it is recaptured every round), otherwise the next free one
        auto slotFor = [&](uint32_t id, uint32_t keep, uint32_t& gen) -> uint32_t {
            if (keep != ~0u && live.geoSlotOwner[keep] == id)
            {
                gen = live.geoSlotGen[keep];
                return keep;
            }
            for (uint32_t tries = 0; tries < kLiveGeoMaxInstances; tries++)
            {
                const uint32_t k = live.geoSlotNext++ % kLiveGeoMaxInstances;
                const uint32_t owner = live.geoSlotOwner[k];
                const auto e = owner ? live.geo.find(owner) : live.geo.end();
                if (owner == 0 || e == live.geo.end() || e->second.slot != k)
                {
                    live.geoSlotOwner[k] = id, gen = ++live.geoSlotGen[k];
                    return k;
                }
            }
            return ~0u;
        };
        for (const LiveRegion& r : probesNow)
        {
            const uint32_t n = r.capacity / 16 / 3 * 3;
            if (n < 9 || !r.id)
                continue;
            if (geoRegions.size() >= kLiveGeoMaxBuilds || frameVerts + n > kLiveGeoFrameVerts)
                break;
            const bool isNew = !live.geo.count(r.id);
            if (live.geo.size() >= kLiveGeoMaxInstances && isNew)
                continue;
            // object closer than 6 m to the camera (the character it follows, its gear, what it carries): never in the cache, where it
            // stayed frozen at its old place (reflection or shadow of a part of the character next to them)
            if (const auto o = live.objects.find(r.id); o != live.objects.end() && o->second.minZ < 6.0f)
            {
                if (!isNew)
                    geoErase(r.id);
                live.statGeoNear++;
                continue;
            }
            // this frame's share: new objects, then refreshes (the others wait for their next round)
            if (isNew ? newVerts + n > live.geoNewVerts : refreshVerts + n > live.geoRefreshVerts)
                continue;
            (isNew ? newVerts : refreshVerts) += n;
            D3D12_RAYTRACING_GEOMETRY_DESC gd = {};
            gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            gd.Triangles.VertexBuffer = { viewVa + uint64_t(frameVerts) * 12, 12 };
            gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            gd.Triangles.VertexCount = n;
            geoms.push_back(gd);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bb = {};
            bb.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            bb.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            bb.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            bb.Inputs.NumDescs = 1;
            bb.Inputs.pGeometryDescs = &geoms.back();
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
            device->GetRaytracingAccelerationStructurePrebuildInfo(&bb.Inputs, &info);
            const uint64_t size = (info.ResultDataMaxSizeInBytes + 255) & ~255ull, scratch = (info.ScratchDataSizeInBytes + 255) & ~255ull;
            if (scratchUsed + scratch > kLiveGeoScratchBytes || size > live.geoPoolBytes / 4)
            {
                geoms.pop_back();
                continue;
            }
            // place in the reserve: ring buffer, the oldest objects in the way make room
            uint32_t keepSlot = ~0u;
            if (const auto e = live.geo.find(r.id); e != live.geo.end())
                keepSlot = e->second.slot;
            geoErase(r.id);
            // the ring buffer's head skips recently captured objects (still on screen or close): in motion, it went round the
            // reserve in a few seconds and evicted visible objects on the way, which were missing from reflections until their next capture.
            // Only objects not recaptured for a long time make room; a full round without room: the old rule
            {
                bool wrapped = false;
                for (uint32_t guard = 0; guard < 256; guard++)
                {
                    if (live.geoHead + size > live.geoPoolBytes)
                    {
                        if (wrapped)
                        {
                            live.geoHead = 0;
                            break;
                        }
                        live.geoHead = 0, wrapped = true;
                    }
                    const uint64_t a = live.geoHead, b = a + size;
                    auto jt = live.geoByOffset.upper_bound(a);
                    if (jt != live.geoByOffset.begin())
                    {
                        auto before = std::prev(jt);
                        if (const auto e = live.geo.find(before->second); e != live.geo.end() && before->first + e->second.size > a)
                            jt = before;
                    }
                    uint64_t skipTo = 0;
                    for (; jt != live.geoByOffset.end() && jt->first < b; ++jt)
                        if (const auto e = live.geo.find(jt->second); e != live.geo.end() && e->second.frame + 600 > live.frame)
                        {
                            skipTo = jt->first + e->second.size;
                            break;
                        }
                    if (!skipTo)
                        break;
                    live.geoHead = skipTo;
                }
            }
            const uint64_t lo = live.geoHead, hi = lo + size;
            auto it = live.geoByOffset.upper_bound(lo);
            if (it != live.geoByOffset.begin())
            {
                auto before = std::prev(it);
                if (const auto e = live.geo.find(before->second); e != live.geo.end() && before->first + e->second.size > lo)
                    it = before;
            }
            while (it != live.geoByOffset.end() && it->first < hi)
            {
                live.geo.erase(it->second);
                it = live.geoByOffset.erase(it);
                live.statGeoEvicted++;
            }
            uint32_t gen = 0;
            const uint32_t slot = slotFor(r.id, keepSlot, gen);
            live.geo[r.id] = { lo, size, live.frame, n / 3, slot, gen };
            live.geoByOffset[lo] = r.id;
            live.geoHead = hi;
            bb.DestAccelerationStructureData = poolVa + lo;
            bb.ScratchAccelerationStructureData = scratchVa + scratchUsed;
            builds.push_back(bb);
            geoRegions.push_back({ r.offset, r.capacity, r.counter, frameVerts * 12 });
            frameVerts += n, scratchUsed += scratch;
        }
        ID3D12Resource* geoTable = geoRegions.empty() ? nullptr : upload(live.geoRegionUpload[live.frame % 3], geoRegions.data(), geoRegions.size() * sizeof(LiveRegion));
        if (geoTable)
        {
            D3D12_RESOURCE_BARRIER gb = {};
            gb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            gb.UAV.pResource = live.motionBuf.Get();
            list->ResourceBarrier(1, &gb);
            list->SetComputeRootSignature(live.geoconvRs.Get());
            list->SetPipelineState(live.geoconvPso.Get());
            list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, geoTable->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.geoView->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, live.motionBuf->GetGPUVirtualAddress());
            const struct { float p00, p11; uint32_t count, pad; } gc = { live.p00, live.p11, uint32_t(geoRegions.size()), 0 };
            list->SetComputeRoot32BitConstants(5, 4, &gc, 0);
            list->Dispatch(UINT(geoRegions.size()), 1, 1);
            Transition(list.Get(), live.geoView.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (const auto& bb : builds)
                list->BuildRaytracingAccelerationStructure(&bb, 0, nullptr);
            Transition(list.Get(), live.geoView.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gb.UAV.pResource = live.geoPool.Get();
            list->ResourceBarrier(1, &gb);
        }
        else if (!geoRegions.empty())
            LiveGeoFlush("upload memory refused");
        live.statGeoFrames++, live.statGeoBuilds += builds.size(), live.statGeoVerts += frameVerts;
    }
    LiveTimeD(list.Get(), 4);
    // 1a. measurement of each captured object, of the RT scene and of the measurement area (geometry budget)
    if (live.statsPso && !(regionsNow.empty() && probesNow.empty()))
    {
        const size_t n = regionsNow.size() + probesNow.size();
        const uint64_t bytes = uint64_t(n) * 16;
        if (live.statsBytes < bytes)
        {
            const uint64_t size = std::max<uint64_t>(bytes * 2, 65536);
            Retire(live.statsBuf);
            live.statsBuf = UavBuffer(g.device, size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (int k = 0; k < 3; k++)
            {
                Retire(live.statsUpload[k]), Retire(live.statsReadback[k]);
                live.statsUpload[k] = MakeBuffer(g.device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                live.statsReadback[k] = MakeBuffer(g.device, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                live.statsIds[k].clear();
            }
            live.statsBytes = size;
        }
        ID3D12Resource* up = live.statsUpload[live.frame % 3].Get();
        void* p = nullptr;
        if (live.statsBuf && up && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            for (size_t i = 0; i < regionsNow.size(); i++)
            {
                const LiveRegion r = { srcNow[i], regionsNow[i].capacity, regionsNow[i].counter, regionsNow[i].id };
                std::memcpy(static_cast<char*>(p) + i * sizeof(LiveRegion), &r, sizeof(r));
            }
            std::memcpy(static_cast<char*>(p) + regionsNow.size() * sizeof(LiveRegion), probesNow.data(), probesNow.size() * sizeof(LiveRegion));
            up->Unmap(0, nullptr);
            list->SetComputeRootSignature(live.statsRs.Get());
            list->SetPipelineState(live.statsPso.Get());
            list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, up->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.statsBuf->GetGPUVirtualAddress());
            const struct { float p00, p11; uint32_t count, pad; } sc = { live.p00, live.p11, uint32_t(n), 0 };
            list->SetComputeRoot32BitConstants(4, 4, &sc, 0);
            list->Dispatch(UINT(n), 1, 1);
            Transition(list.Get(), live.statsBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.statsReadback[live.frame % 3].Get(), 0, live.statsBuf.Get(), 0, bytes);
            Transition(list.Get(), live.statsBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            std::vector<uint32_t>& ids = live.statsIds[live.frame % 3];
            std::vector<uint32_t>& caps = live.statsCaps[live.frame % 3];
            ids.clear(), caps.clear();
            for (const LiveRegion& r : regionsNow)
                ids.push_back(r.id), caps.push_back(r.capacity / 16);
            for (const LiveRegion& r : probesNow)
                ids.push_back(r.id), caps.push_back(r.capacity / 16);
        }
        live.statDraws += regionsNow.size(), live.statProbes += probesNow.size(), live.statBudgetFrames++;
    }
    // 1g. diagnostic (motion_log=1): flatness of small captured objects, every eight computations; summary read three frames later
    if (live.objplanesPso && live.motionLog)
    {
        std::vector<uint32_t>& flatIds = live.objPlanesIds[live.frame % 3];
        if (const float* f = nullptr; !flatIds.empty() && live.objPlanesRead[live.frame % 3] && SUCCEEDED(live.objPlanesRead[live.frame % 3]->Map(0, nullptr, (void**)&f)) && f)
        {
            for (size_t k = 0; k < flatIds.size(); k++)
            {
                const uint32_t tris = reinterpret_cast<const uint32_t*>(f)[k * 8];
                const float coh = f[k * 8 + 1], err = f[k * 8 + 2], size = f[k * 8 + 3], ny = f[k * 8 + 5];
                const auto m = live.idMesh.find(flatIds[k]);
                if (m == live.idMesh.end())
                    continue;
                if (live.motionLog && tris >= 2 && size >= 8.0f && std::fabs(ny) > 0.8f && coh > 0.8f && err < 0.03f * size + 0.1f && live.flatLogged.insert(m->second).second && live.flatLogged.size() < 400)
                    Log("FLATOBJ mesh %08x triangles %u coherence %.3f deviation %.2f size %.1f normal %.2f %.2f %.2f distance %.2f", m->second, tris, coh, err, size, f[k * 8 + 4], ny, f[k * 8 + 6], f[k * 8 + 7]);
            }
            live.objPlanesRead[live.frame % 3]->Unmap(0, nullptr);
        }
        flatIds.clear();
        if (live.frame % 8 == 0)
        {
            std::vector<LiveRegion> smallObjs;
            for (size_t i = 0; i < regionsNow.size(); i++)
                if (regionsNow[i].capacity / 16 <= 6000)
                    smallObjs.push_back({ srcNow[i], regionsNow[i].capacity, regionsNow[i].counter, regionsNow[i].id });
            for (const LiveRegion& r : probesNow)
                if (r.capacity / 16 <= 6000)
                    smallObjs.push_back(r);
            const uint64_t planeBytes = uint64_t(smallObjs.size()) * 32;
            if (live.objPlanesBytes < planeBytes)
            {
                Retire(live.objPlanes);
                live.objPlanesBytes = std::max<uint64_t>(planeBytes * 2, 65536);
                live.objPlanes = UavBuffer(g.device, live.objPlanesBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                for (auto& rb : live.objPlanesRead)
                    Retire(rb), rb = MakeBuffer(g.device, live.objPlanesBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                for (auto& v : live.objPlanesIds)
                    v.clear();
            }
            ID3D12Resource* smallBuf = smallObjs.empty() ? nullptr : upload(live.objPlanesUpload[live.frame % 3], smallObjs.data(), smallObjs.size() * sizeof(LiveRegion));
            if (smallBuf && live.objPlanes && live.objPlanesRead[live.frame % 3])
            {
                list->SetComputeRootSignature(live.objplanesRs.Get());
                list->SetPipelineState(live.objplanesPso.Get());
                list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
                list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(2, smallBuf->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(3, live.objPlanes->GetGPUVirtualAddress());
                const struct { float p00, p11; uint32_t count, pad; } oc = { live.p00, live.p11, uint32_t(smallObjs.size()), 0 };
                list->SetComputeRoot32BitConstants(4, 4, &oc, 0);
                list->Dispatch(UINT(smallObjs.size()), 1, 1);
                Transition(list.Get(), live.objPlanes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyBufferRegion(live.objPlanesRead[live.frame % 3].Get(), 0, live.objPlanes.Get(), 0, planeBytes);
                Transition(list.Get(), live.objPlanes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                for (const LiveRegion& r : smallObjs)
                    flatIds.push_back(r.id);
            }
        }
    }
    Transition(list.Get(), live.view.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);

    LiveTime(list.Get(), 1);
    LiveTimeD(list.Get(), 5);
    // 1b. sun of this frame: pairs screen / shadow map with the same signature, fitted then voted
    if (live.fovReset)
    {
        // projection changed: the stretch measurement starts from zero
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.sunBuf.Get(), 1200, g.zeros.Get(), 0, 24);
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.fovReset = false;
    }
    if (live.sunDirty)
    {
        void* p = nullptr;
        if (SUCCEEDED(live.sunUpload->Map(0, nullptr, &p)))
        {
            const float s[12] = { live.sun[0], live.sun[1], live.sun[2], 1, 0, 0, 0, 0, 0, live.A, live.B, std::isfinite(live.A) && std::isfinite(live.B) && live.B != 0 ? 1.0f : 0.0f };
            std::memcpy(p, s, sizeof(s));
            live.sunUpload->Unmap(0, nullptr);
        }
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.sunBuf.Get(), 0, live.sunUpload.Get(), 0, 48);
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.sunDirty = false;
    }
    struct Pair
    {
        uint32_t camBase, mapBase, camCounter, mapCounter; // starts in the stream (bytes), counters
        float viewport[4];
        float weight;
        uint32_t camCapacity, mapCapacity, pad;
    };
    std::vector<Pair> pairs;
    {
        std::unordered_map<uint64_t, std::vector<uint32_t>> sunByKey;
        for (uint32_t i = 0; i < marksNow.size(); i++)
            if (marksNow[i].sunMap)
                sunByKey[marksNow[i].key].push_back(i);
        std::unordered_map<uint64_t, uint32_t> rank;
        for (uint32_t i = 0; i < marksNow.size() && pairs.size() < kLiveMaxPairs; i++)
        {
            if (marksNow[i].sunMap)
                continue;
            auto it = sunByKey.find(marksNow[i].key);
            if (it == sunByKey.end())
                continue;
            const uint32_t r = rank[marksNow[i].key]++;
            const std::vector<uint32_t>& c = it->second;
            // same rank in both passes: very probably the same object (weight 8)
            auto add = [&](uint32_t m, float w) {
                const LiveMark &a = marksNow[i], &b = marksNow[m];
                pairs.push_back({ uint32_t(a.slot * kSlotBytes + a.offset), uint32_t(b.slot * kSlotBytes + b.offset), a.counter, b.counter,
                                  { b.viewport[0], b.viewport[1], b.viewport[2], b.viewport[3] }, w, a.capacity, b.capacity, 0 });
            };
            if (r < c.size())
                add(c[r], 8.0f);
            for (uint32_t k = 0; k < c.size() && k < 3 && pairs.size() < kLiveMaxPairs; k++)
                if (k != r)
                    add(c[k], 1.0f);
        }
    }
    {
        void* p = nullptr;
        ID3D12Resource* up = live.pairsUpload[live.frame % 3].Get();
        if (!pairs.empty() && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            std::memcpy(p, pairs.data(), pairs.size() * sizeof(Pair));
            up->Unmap(0, nullptr);
            list->SetComputeRootSignature(live.fitRs.Get());
            list->SetPipelineState(live.fitPso.Get());
            list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, up->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.pairDirs->GetGPUVirtualAddress());
            const struct { float p00, p11; uint32_t count, slotBytes; } fc = { live.p00, live.p11, uint32_t(pairs.size()), uint32_t(kSlotBytes) };
            list->SetComputeRoot32BitConstants(4, 4, &fc, 0);
            list->Dispatch(UINT(pairs.size()), 1, 1);
            Transition(list.Get(), live.pairDirs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.voteRs.Get());
            list->SetPipelineState(live.votePso.Get());
            list->SetComputeRootShaderResourceView(0, live.pairDirs->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(1, live.sunBuf->GetGPUVirtualAddress());
            const struct { uint32_t count; float cosTol, minShare, sunSign, jitterDelta, jitterAbsolute, pad0, pad1; } vc = {
                uint32_t(pairs.size()), std::cos(2.0f * 3.14159265f / 180.0f), 0.35f, live.sunSign, live.jitterMode >= 1 ? 1.0f : 0.0f,
                live.jitterMode >= 2 ? 1.0f : 0.0f, 0, 0 };
            list->SetComputeRoot32BitConstants(2, 8, &vc, 0);
            list->Dispatch(1, 1, 1);
            Transition(list.Get(), live.pairDirs.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            // no pair: "not measured" summary, the previous sun stays, no known cascade any more
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.sunBuf.Get(), 16, g.zeros.Get(), 0, 20); // not the depth law (36 to 47)
            list->CopyBufferRegion(live.sunBuf.Get(), 624, g.zeros.Get(), 0, 32); // rotation not measured, no cascades kept any more
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    // sub-pixel jitter: the game camera's, every frame. The measurement on the cascades exists on only one frame in
    // five in some games: the rest of the time, the frame-to-frame difference was 0 and the absolute value stayed frozen, mask and
    // reflections laid next to the image by a varying half pixel, history read at the wrong place (flickering foliage)
    if (trueCam && live.jitterMode >= 1)
    {
        // (with the camera rotation, from this frame to the previous one, same place as live_sunvote: reflection stabilisation uses it
        // to know where the reflection was; measured on the cascades, it was missing nine frames out of ten, and the history was read off-target)
        const float jit[17] = { camData[0], camData[3], camData[6], 0.0f, camData[1], camData[4], camData[7], 0.0f, camData[2], camData[5], camData[8], 0.0f, 1.0f,
                                shearPrev[0] - camData[44], shearPrev[1] - camData[45], camData[44], camData[45] };
        if (ID3D12Resource* up = upload(live.jitUpload[live.frame % 3], jit, sizeof(jit)))
        {
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.sunBuf.Get(), 576, up, 0, 60);
            if (live.jitterMode >= 2)
                list->CopyBufferRegion(live.sunBuf.Get(), 1184, up, 60, 8);
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    // 1c. depth law of this frame, measured on the screen's vertices
    // A law already trusted (stored, checked) takes precedence: the measurement averages the captured vertices, and they do not all
    // follow the same law (in one game: 0.19 / w for the scenery, something else for the character). The frame's scene keeping only what is
    // near, it fell between 0.08 and 0.13 from one frame to the next: every distance wrong, the reflection dropping
    // across the whole lake for series of frames. urti.ini depth_law_fixed=: 1 always the known law, 0 always the measurement
    const bool lawKnown = live.depthLawFixed == 1 || (live.depthLawFixed < 0 && (live.fromStore || live.verified));
    ID3D12Resource* lawUp = nullptr;
    if (lawKnown)
    {
        const float law[4] = { live.A, live.B, 1.0f, 0.0f };
        lawUp = upload(live.lawUpload[live.frame % 3], law, sizeof(law));
    }
    if (lawUp)
    {
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.sunBuf.Get(), 36, lawUp, 0, 12);
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else
    {
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = live.sunBuf.Get();
        list->ResourceBarrier(1, &ub);
        list->SetComputeRootSignature(live.depthfitRs.Get());
        list->SetPipelineState(live.depthfitPso.Get());
        list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
        list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, fitBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3, live.sunBuf->GetGPUVirtualAddress());
        const uint32_t dc[4] = { uint32_t(fitNow.size()), fitEnd, 0, 0 };
        list->SetComputeRoot32BitConstants(4, 4, dc, 0);
        list->Dispatch(1, 1, 1);
    }
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);

    LiveTimeD(list.Get(), 6);
    // 2. acceleration structures
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer = { live.view->GetGPUVirtualAddress(), 12 };
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = converted;
    // same list of draws (regions, sizes, identities) as at the last build: the triangles are
    // the same, only their positions changed (camera, wind, moving objects): update
    uint64_t layout = 1469598103934665603ull ^ converted;
    for (const LiveRegion& r : regionsNow)
        layout = (layout ^ (uint64_t(r.offset) << 32 ^ r.capacity ^ uint64_t(r.id) << 17)) * 1099511628211ull;
    const bool refit = live.blasRefit > 0 && live.blasAge < live.blasRefit &&
                       (persistNow ? converted == live.blasConverted && !live.placesMoved && live.refitExtra < live.rebuildMs - live.refitMs
                                   : layout == live.blasLayout);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                      (refit ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(0));
    if (refit)
        bd.SourceAccelerationStructureData = live.blas->GetGPUVirtualAddress(), live.blasAge++, live.statUpdates++;
    else
        live.blasLayout = layout, live.blasConverted = converted, live.blasAge = 0, live.statRebuilds++,
        live.buildCycle++, live.refitExtra = 0.0, live.traceBase = -1.0;
    live.slotCycle[live.frame % 3] = live.buildCycle, live.slotRefit[live.frame % 3] = refit, live.slotBuilt[live.frame % 3] = true;
    live.statVertices += converted;
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.NumDescs = 1;
    bd.Inputs.pGeometryDescs = &geom;
    bd.DestAccelerationStructureData = live.blas->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = live.blasScratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    LiveTimeD(list.Get(), 7);
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = live.blas.Get();
    list->ResourceBarrier(1, &uav);
    uint32_t tlasCount = 2;
    D3D12_GPU_VIRTUAL_ADDRESS tlasInstances = live.instances->GetGPUVirtualAddress();
    {
        // copy 0: the opaque scene (mask 1: occlusion, reflections, cache); copy 1: the surfaces
        // flat transparent surfaces (mask 2: only the reflections' camera ray looks for them; mask 0 without them)
        D3D12_RAYTRACING_INSTANCE_DESC inst[2] = {};
        for (auto& i : inst)
            i.Transform[0][0] = i.Transform[1][1] = i.Transform[2][2] = 1.0f;
        inst[0].InstanceMask = 0x01;
        inst[0].AccelerationStructure = live.blas->GetGPUVirtualAddress();
        inst[1].InstanceID = 1;
        inst[1].InstanceMask = live.transpBuilt ? 0x02 : 0x00;
        inst[1].AccelerationStructure = live.transpBuilt ? live.blasT->GetGPUVirtualAddress() : live.blas->GetGPUVirtualAddress();
        void* p = nullptr;
        if (SUCCEEDED(live.instances->Map(0, nullptr, &p)))
        {
            std::memcpy(p, inst, sizeof(inst));
            live.instances->Unmap(0, nullptr);
        }
        // geometry cache copies: same mask as the frame's scene, identity 2 (neither regions nor vertices
        // of a previous frame), placed with this frame's camera pose (live_geoinst)
        if (geoActive)
        {
            std::vector<D3D12_RAYTRACING_INSTANCE_DESC> all(2 + live.geo.size());
            all[0] = inst[0], all[1] = inst[1];
            size_t k = 2;
            const D3D12_GPU_VIRTUAL_ADDRESS poolVa = live.geoPool->GetGPUVirtualAddress();
            for (const auto& [id, e] : live.geo)
            {
                D3D12_RAYTRACING_INSTANCE_DESC& d = all[k++];
                // identity: bit 23, generation of the place, place (live_surfcache, live_reflect); the object's triangles in the hit-group field
                // (unused by inline rays)
                d.InstanceID = e.slot != ~0u ? (1u << 23) | ((e.gen & 255u) << 14) | e.slot : 2u;
                d.InstanceContributionToHitGroupIndex = std::min<uint32_t>(e.triangles, 0xFFFFFF);
                d.InstanceMask = 0x01;
                d.AccelerationStructure = poolVa + e.offset;
            }
            ID3D12Resource* geoUp = upload(live.geoUpload[live.frame % 3], all.data(), all.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            if (geoUp)
            {
                Transition(list.Get(), live.geoInstances.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyBufferRegion(live.geoInstances.Get(), 0, geoUp, 0, all.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
                Transition(list.Get(), live.geoInstances.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                list->SetComputeRootSignature(live.geoinstRs.Get());
                list->SetPipelineState(live.geoinstPso.Get());
                list->SetComputeRootUnorderedAccessView(0, live.geoInstances->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(1, live.motionBuf->GetGPUVirtualAddress());
                const uint32_t ic[4] = { 2, uint32_t(all.size()), 0, 0 };
                list->SetComputeRoot32BitConstants(2, 4, ic, 0);
                list->Dispatch(UINT((all.size() + 63) / 64), 1, 1);
                Transition(list.Get(), live.geoInstances.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                tlasCount = uint32_t(all.size());
                tlasInstances = live.geoInstances->GetGPUVirtualAddress();
            }
        }
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    td.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    td.Inputs.NumDescs = tlasCount;
    td.Inputs.InstanceDescs = tlasInstances;
    td.DestAccelerationStructureData = live.tlas->GetGPUVirtualAddress();
    td.ScratchAccelerationStructureData = live.tlasScratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    LiveTimeD(list.Get(), 8);
    uav.UAV.pResource = live.tlas.Get();
    list->ResourceBarrier(1, &uav);

    // 3a. previous frame's depth kept (accumulation over time)
    if (live.depthPrevBuf)
    {
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(live.depthPrevBuf.Get(), live.depthBuf.Get());
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    // 3. the game's depth (ReShade's copy) into a buffer the shader can read
    const D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool snapUse = live.syncNow && live.snapIndexNow >= 0 && live.depthSnap[live.snapIndexNow] && live.depthSnapPitch == live.depthPitch &&
                         live.depthSnapWidth == depthDesc.Width && live.depthSnapHeight == depthDesc.Height;
    if (snapUse)
    {
        // this frame's depth, copied inside the game's list (executed: wait on its queue above)
        live.statSnapUsed++;
        list->CopyBufferRegion(live.depthBuf.Get(), 0, live.depthSnap[live.snapIndexNow].Get(), 0, std::min<uint64_t>(live.depthSnapBytes, live.depthBuf->GetDesc().Width));
    }
    else
    {
        live.statSnapFallback++;
        Transition(list.Get(), depthCopy, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        g.device->GetCopyableFootprints(&depthDesc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = live.depthBuf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = depthCopy;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list.Get(), depthCopy, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);
    }
    Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    // 3c. motion vectors built from the measured camera motion (the game's are unreadable)
    if (live.mvSynthetic && live.mvsynthPso && live.motionBuf)
    {
        const uint32_t pitch = (live.width * 4 + 255) & ~255u;
        const uint64_t bytes = uint64_t(pitch) * live.height;
        if (!live.mvBuf || live.mvBytes < bytes)
        {
            Retire(live.mvBuf);
            live.mvBuf = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
            live.mvBytes = live.mvBuf ? bytes : 0;
            Log("URTI live: built motion vectors %ux%u (camera motion measured on our geometry)", live.width, live.height);
        }
        if (live.mvBuf)
        {
            const bool d24m = depthDesc.Format == DXGI_FORMAT_R24G8_TYPELESS || depthDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
            Transition(list.Get(), live.mvBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list.Get(), live.motionBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.mvsynthRs.Get());
            list->SetPipelineState(live.mvsynthPso.Get());
            list->SetComputeRootShaderResourceView(0, live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, live.motionBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.mvBuf->GetGPUVirtualAddress());
            const struct { uint32_t width, height, depthPitch, depth24, mvPitch; float p00, p11; uint32_t pad; } mc = {
                live.width, live.height, live.depthPitch, d24m ? 1u : 0u, pitch, live.p00, live.p11, 0 };
            list->SetComputeRoot32BitConstants(4, 8, &mc, 0);
            list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
            Transition(list.Get(), live.motionBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list.Get(), live.mvBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            live.mvPitch = pitch, live.mvWidth = live.width, live.mvHeight = live.height, live.mvFrame = live.frame, live.mvConvention = 0;
        }
    }

    LiveTime(list.Get(), 2);
    // 3b. the game's shadow map (its shadow is recomputed pixel by pixel: our shadows are only added
    // where the game puts none)
    bool atlasValid = false;
    // (also read to recognise holes in cut-out foliage: the map is the sun's view)
    if (ID3D12Resource* atlas = reinterpret_cast<ID3D12Resource*>(g_liveSunDepth); atlas && (g_liveCombine || live.alphaScreen) && live.calibrated)
    {
        uint32_t state = 0;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g.depthStates.find(g_liveSunDepth);
            if (it != g.depthStates.end())
                state = PassD3D12State(it->second), known = true;
        }
        const D3D12_RESOURCE_DESC ad = atlas->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT afp = {};
        UINT64 atotal = 0;
        g.device->GetCopyableFootprints(&ad, 0, 1, 0, &afp, nullptr, nullptr, &atotal);
        if (known && (!live.atlasBuf || live.atlasBytes < atotal))
        {
            Retire(live.atlasBuf);
            live.atlasBuf = UavBuffer(g.device, atotal, D3D12_RESOURCE_STATE_COPY_DEST);
            live.atlasBytes = atotal;
            Log("URTI live: the game's shadow map %llux%u format %d copied every frame (%.0f MB)", (unsigned long long)ad.Width, ad.Height,
                int(ad.Format), atotal / 1048576.0);
        }
        if (known && live.atlasBuf)
        {
            live.atlasPitch = afp.Footprint.RowPitch;
            live.atlasD24 = ad.Format == DXGI_FORMAT_R24G8_TYPELESS || ad.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
            live.atlas16 = ad.Format == DXGI_FORMAT_R16_TYPELESS || ad.Format == DXGI_FORMAT_D16_UNORM || ad.Format == DXGI_FORMAT_R16_UNORM;
            Transition(list.Get(), atlas, D3D12_RESOURCE_STATES(state), D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = live.atlasBuf.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = afp;
            src.pResource = atlas;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list.Get(), atlas, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(state));
            Transition(list.Get(), live.atlasBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            atlasValid = true;
            live.atlasFrame = live.frame;
        }
    }

    LiveTime(list.Get(), 3);
    LiveTimeD(list.Get(), 9);
    // previous frame's mask kept (generated frames, blend mode)
    if (live.prevShadowBuf)
    {
        Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.prevShadowBuf.Get(), 0, live.shadowBuf.Get(), 0, uint64_t(live.shadowPitch) * live.height);
        Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // light bounce: quarter-rate occlusion rays, surface cache filled and covering the whole scene, region table
    // in video memory
    // menu: occlusion and bounce each off or scaled; the bounce needs the occlusion rays (cast even when it is off, almost without effect)
    const float aoEff = live.aoAmount * (live.menuAo ? live.menuAoForce : 0.0f), giEff = live.giStrength * (live.menuGi ? live.menuGiForce : 0.0f);
    const float aoRadiusNow = live.menuAoRadius > 0 ? live.menuAoRadius : live.aoRadius;
    const bool giNow = giEff > 0 && live.aoAmount > 0 && live.aoQuarter && !live.sunShadows && live.shadowRawBuf && live.denoisePso &&
                       live.giRaw && live.giBuf && live.giPrev && live.surfCacheOn && live.surfCache && live.surfFilled > 0 &&
                       live.surfTriangles >= live.capacity / 3 && live.sceneGpuReadable && sceneTable == live.sceneGpu.Get();
    if (giNow && live.giValid)
    {
        Transition(list.Get(), live.giBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.giPrev.Get(), 0, live.giBuf.Get(), 0, 3ull * live.shadowPitch * live.height);
        Transition(list.Get(), live.giBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (giNow)
        Transition(list.Get(), live.surfCache.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    // 4. rays from each pixel (sun, occlusion)
    list->SetComputeRootSignature(live.shadowRs.Get());
    list->SetPipelineState(live.shadowPso.Get());
    list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, live.sunBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, atlasValid ? live.atlasBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    const bool motionOk = live.mvBuf && live.mvFrame != ~0ull && live.frame <= live.mvFrame + 1 && live.mvWidth >= live.width &&
                          live.mvHeight >= live.height && live.depthPrevBuf;
    if (motionOk)
    {
        Transition(list.Get(), live.mvBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
        Transition(list.Get(), live.depthPrevBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    }
    if (live.prevShadowBuf)
        Transition(list.Get(), live.prevShadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(4, live.prevShadowBuf ? live.prevShadowBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(5, motionOk ? live.depthPrevBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(6, motionOk ? live.mvBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(7, giNow ? live.surfCache->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(8, giNow ? sceneTable->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    // (root: 11 reads t0-t10, then u0 raw mask, u1 penumbra, u2 bounce, u3 normal comparison, then the constants)
    list->SetComputeRootUnorderedAccessView(11, live.shadowRawBuf ? live.shadowRawBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(13, giNow ? live.giRaw->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    // this frame's G-buffer normals (ring: the slot of the frame's material; otherwise the last copy)
    ID3D12Resource* gnNow = nullptr;
    if (live.gnMode && live.gnFrame != ~0ull && live.frame <= live.gnFrame + 8 && live.gnWidth >= live.width && live.gnHeight >= live.height)
        gnNow = live.frameSync ? (live.materialIndexNow >= 0 ? live.gnSnap[live.materialIndexNow].Get() : nullptr) : live.gnBuf.Get();
    if (gnNow && !live.frameSync)
        Transition(list.Get(), gnNow, D3D12_RESOURCE_STATE_COPY_DEST, srv);
    const bool gnStatsNow = gnNow && live.gnCompare;
    const bool gnRotNow = gnNow && (live.gnMode & 3) == 2 && live.rotfitPso;
    if ((gnStatsNow || gnRotNow) && !live.gnStats)
    {
        live.gnStats = UavBuffer(g.device, 128, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.gnStatsRb = MakeBuffer(g.device, 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (gnRotNow && live.gnStats)
    {
        Transition(list.Get(), live.gnStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.gnStats.Get(), 64, g.zeros.Get(), 0, 64);
        Transition(list.Get(), live.gnStats.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    list->SetComputeRootShaderResourceView(9, gnNow ? gnNow->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    // Red Dead's material target (world normals: same G-buffer layout): its type keeps foliage and skin out of occlusion
    ID3D12Resource* matShadow = nullptr;
    if ((live.gnMode & 3) == 2 && live.materialFrame != ~0ull && live.frame <= live.materialFrame + 8 && live.materialPitch < 65536 &&
        live.materialWidth >= live.width && live.materialHeight >= live.height)
        matShadow = live.frameSync ? (live.materialIndexNow >= 0 ? live.materialSnap[live.materialIndexNow].Get() : nullptr) : live.materialBuf.Get();
    if (matShadow && !live.frameSync)
        Transition(list.Get(), matShadow, D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(10, matShadow ? matShadow->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(14, (gnStatsNow || gnRotNow) && live.gnStats ? live.gnStats->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    const bool d24 = depthDesc.Format == DXGI_FORMAT_R24G8_TYPELESS || depthDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    // SIGMA (NVIDIA): one ray per pixel, penumbra and weight of the corrections; denoising afterwards
#if URTI_NRD
    const bool nrdActive = live.nrdShadows && motionOk && live.mvConvention < 16 && live.shadowRawBuf && NrdEnsure(runtime);
    list->SetComputeRootUnorderedAccessView(12, nrdActive ? g_nrd.penumbraBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
#else
    const bool nrdActive = false;
    list->SetComputeRootUnorderedAccessView(12, live.shadowBuf->GetGPUVirtualAddress());
#endif
    // sun measured within the last 30 frames (3 of readback lag): otherwise no RT shadows
    // overcast, dusk: the sun share measured in the image is so small that an RT shadow would not
    // show (less than 13 % of the light removed): no shadow rays (several ms saved)
    const bool sunWeak = live.sunMeasure && live.sunSkyFrames && live.sunSky[3] < 0.15f;
    if (sunWeak != live.sunWeakLogged)
        Log("URTI live: RT shadows %s (measured sun / sky %.2f)", sunWeak ? "suspended: direct light too weak to show" : "resumed", live.sunSky[3]);
    live.sunWeakLogged = sunWeak;
    const bool sunNow = live.calibrated && live.framesSinceSun < 30 && !sunWeak;
    // (without a sun, the pass keeps ambient occlusion: flag 16 only cuts the sun rays)
    const uint32_t flags = (atlasValid ? 1u : 0u) | (live.atlasD24 ? 2u : 0u) | (live.atlas16 ? 32u : 0u) | (g_liveCombine ? 4u : 0u) | (g_liveDebugGame ? 8u : 0u) |
                           (sunNow ? 0u : 16u) | (live.sunShadows ? 0u : 64u) | (live.shadowRays << 8) | (live.alphaScreen ? 2048u : 0u) |
                           (nrdActive ? 4096u : 0u) |
                           (live.aoQuarter && !live.sunShadows && !nrdActive && live.shadowRawBuf && live.denoisePso ? 128u | (live.aoRays << 16) : 0u) |
                           (giNow && !nrdActive ? 1u << 20 : 0u) | (live.aoRough ? 1u << 21 : 0u) | (live.slowRays ? 1u << 23 : 0u) | (live.aoHopsNow << 24) |
                           (live.aoIgn ? 1u << 27 : 0u) | (live.aoSunRay ? 1u << 28 : 0u);
    const struct
    {
        float sunSign, gameBias, unused, bias;
        uint32_t width, height, depthPitch, shadowPitch;
        float A, B, p00, p11, clearDepth;
        uint32_t d24, atlasPitch, flags;
        uint32_t motionPitch, motionConvention;
        float temporalAlpha;
        uint32_t debugMode, frameIndex;
        float sunSky;
        uint32_t gameDilate, gameMin;
        float aoAmount, aoRadius;
        uint32_t aoMotion, cacheTriangles, regionCount;
        float aoMaxDist;
        uint32_t gnMode, gnPitch;
        float skyAmount, skyRange;
    } sc = { live.sunSign, g_liveGameBias, g_liveSunRadius * 3.14159265f / 180.0f, g_liveRayBias, live.width, live.height, live.depthPitch, live.shadowPitch,
             live.A, live.B, live.p00, live.p11, g.depthReversed ? 0.0f : 1.0f, d24 ? 1u : 0u, live.atlasPitch, flags,
             live.mvPitch, live.shadowRawBuf ? 255u : (motionOk && live.prevShadowBuf ? live.mvConvention : 255u), live.temporalAlpha,
             // (sun / sky: the measurement in the game's image relies on its shadow map, which does not match in Red Dead: 0.04 in full sun.
             // Below 0.3, implausible: 2.2, URTI's assumed value)
             0u, uint32_t(live.frame), live.sunMeasure && live.sunSky[3] >= 0.3f ? live.sunSky[3] : 2.2f, live.gameDilate, live.gameMin ? 1u : 0u,
             aoEff > 0 || giNow ? std::max(aoEff, 1e-4f) : 0.0f, aoRadiusNow, live.aoMotionFull && motionOk && live.mvConvention < 16 ? live.mvConvention : 255u, // (motion vectors: moving pixels computed every frame)
             giNow ? uint32_t(live.surfTriangles) : 0u, giNow ? uint32_t(tableNow.size()) : 0u, live.aoDistNow,
             gnNow ? (live.gnMode & 3) | (live.gnSigns << 8) | (live.gnUse ? 0x1000u : 0u) | (gnStatsNow && live.gnStats ? 0x2000u : 0u) | (gnRotNow && live.gnStats ? 0x4000u : 0u) : 0u, (gnNow ? live.gnPitch : 0u) | (matShadow ? live.materialPitch << 16 : 0u),
             aoEff > 0 ? live.skyAmount * live.menuSky : 0.0f, live.skyRange };
    list->SetComputeRoot32BitConstants(15, 34, &sc, 0);
    list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
    if (matShadow && !live.frameSync)
        Transition(list.Get(), matShadow, srv, D3D12_RESOURCE_STATE_COPY_DEST);
    live.gnNowFrame = gnNow; // (without a ring: kept readable until after the reflections)
    // normal comparison: read again every 300 frames (copied, reset; read three frames later)
    if (gnStatsNow && live.gnStats && live.gnStatsRb)
    {
        if (live.frame % 300 == 3)
        {
            if (const int32_t* v = nullptr; SUCCEEDED(live.gnStatsRb->Map(0, nullptr, (void**)&v)) && v)
            {
                char line[512];
                int len = std::snprintf(line, sizeof(line), "URTI live: G-buffer normals against depth normals (flat surfaces), mean agreement per sign flip:");
                for (int s = 0; s < 8 && len < int(sizeof(line)) - 40; s++)
                    len += std::snprintf(line + len, sizeof(line) - len, " %s%s%s %.3f (%d)", (s & 1) ? "-x" : "x", (s & 2) ? "-y" : "y", (s & 4) ? "-z" : "z",
                                         v[s * 2 + 1] ? v[s * 2] / 1000.0 / v[s * 2 + 1] : 0.0, v[s * 2 + 1]);
                Log("%s", line);
                live.gnStatsRb->Unmap(0, nullptr);
            }
        }
        if (live.frame % 300 == 0)
        {
            D3D12_RESOURCE_BARRIER sb = {};
            sb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            sb.UAV.pResource = live.gnStats.Get();
            list->ResourceBarrier(1, &sb);
            Transition(list.Get(), live.gnStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.gnStatsRb.Get(), 0, live.gnStats.Get(), 0, 64);
            Transition(list.Get(), live.gnStats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.gnStats.Get(), 0, g.zeros.Get(), 0, 64);
            Transition(list.Get(), live.gnStats.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    LiveTimeD(list.Get(), 10);
    if (giNow)
        Transition(list.Get(), live.surfCache.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // denoising: NVIDIA's SIGMA; otherwise URTI's (depth-guided blur, reprojected history,
    // statistical clamp, adaptive blend)
    bool nrdDone = false;
#if URTI_NRD
    if (nrdActive)
    {
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = nullptr; // both outputs of the shadow trace
        list->ResourceBarrier(1, &ub);
        nrdDone = NrdShadows(runtime, list.Get(), d24, motionOk);
        live.statNrd += nrdDone ? 1 : 0;
    }
#endif
    if (!nrdDone && live.shadowRawBuf && live.denoisePso)
    {
        Transition(list.Get(), live.shadowRawBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        list->SetComputeRootSignature(live.denoiseRs.Get());
        list->SetPipelineState(live.denoisePso.Get());
        list->SetComputeRootShaderResourceView(0, live.shadowRawBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, live.prevShadowBuf ? live.prevShadowBuf->GetGPUVirtualAddress() : live.shadowRawBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, live.depthBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(3, motionOk ? live.depthPrevBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(4, motionOk ? live.mvBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(5, live.sunBuf->GetGPUVirtualAddress());
        // history length: the previous frame's is read, this frame's written into the other buffer
        const bool lenNow = live.denoiseLenOn && live.denoiseLen[0] && live.denoiseLen[1];
        const uint32_t lenOut = live.denoiseLenNext & 1;
        if (lenNow)
        {
            Transition(list.Get(), live.denoiseLen[1 - lenOut].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            live.denoiseLenNext++;
        }
        list->SetComputeRootShaderResourceView(6, lenNow ? live.denoiseLen[1 - lenOut]->GetGPUVirtualAddress() : live.sunBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(7, live.shadowBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(8, lenNow ? live.denoiseLen[lenOut]->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
        const struct
        {
            uint32_t width, height, depthPitch, shadowPitch, motionPitch, motionConvention, depth24, debugMode;
            float alphaMin, alphaMax, gamma;
            uint32_t blurRadius;
            float p00, p11, farSkip;
        } dn = { live.width, live.height, live.depthPitch, live.shadowPitch, live.mvPitch,
                 motionOk && live.prevShadowBuf ? live.mvConvention : 255u, d24 ? 1u : 0u, live.debugView >= 1 && live.debugView <= 3 ? live.debugView : 0u,
                 // ambient occlusion alone (sun shadows off): it varies slowly, more frames
                 // accumulated and a wider clamp (less shaking grain)
                 // (in motion: half new, moving pixels being computed every frame; v104 had this comment in the middle of a line,
                 // which removed the clamp and the blur radius: settings shifted by one place)
                 live.sunShadows ? 0.1f : 0.06f, live.sunShadows || live.aoMotionFull ? 0.5f : 0.3f, live.sunShadows ? 1.5f : 2.0f,
                 (live.sunShadows ? 1u : 2u) | (lenNow ? 2048u : 0u) | (lenNow && live.denoiseDual && !live.sunShadows ? 4096u : 0u),
                 live.p00, live.p11, live.sunShadows || !live.denoiseFarSkip ? 0.0f : live.aoDistNow * 1.02f + 0.5f };
        list->SetComputeRoot32BitConstants(9, 15, &dn, 0);
        list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
        Transition(list.Get(), live.shadowRawBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (giNow && !nrdActive)
        {
            Transition(list.Get(), live.giRaw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            if (live.giValid)
                Transition(list.Get(), live.giPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            list->SetComputeRootShaderResourceView(0, live.giRaw->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.giValid ? live.giPrev->GetGPUVirtualAddress() : live.giRaw->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(7, live.giBuf->GetGPUVirtualAddress());
            auto gn = dn;
            gn.motionConvention = motionOk && live.giValid ? live.mvConvention : 255u;
            gn.debugMode = 0;
            gn.blurRadius = 2u | 256u | 512u | (lenNow ? 2048u | 1024u : 0u) | (lenNow && live.denoiseDual && !live.sunShadows ? 4096u : 0u); // (4096: lengths stored with the fast history) // 5×5, never computed -> 0, three planes; length read (the same as occlusion), not written
            list->SetComputeRoot32BitConstants(9, 15, &gn, 0);
            list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
            Transition(list.Get(), live.giRaw.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (live.giValid)
                Transition(list.Get(), live.giPrev.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER gb = {};
            gb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            gb.UAV.pResource = live.giBuf.Get();
            list->ResourceBarrier(1, &gb);
        }
        if (lenNow)
        {
            Transition(list.Get(), live.denoiseLen[1 - lenOut].Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_RESOURCE_BARRIER lb = {};
            lb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            lb.UAV.pResource = live.denoiseLen[lenOut].Get();
            list->ResourceBarrier(1, &lb);
        }
    }
    live.giValid = giNow && !nrdActive && !nrdDone && live.shadowRawBuf && live.denoisePso;
    live.giStamp = live.frame;
    // search for the motion-vector convention (as long as it is not found)
    if (motionOk && live.mvConvention == 255 && live.mvcalPso)
    {
        if (!live.mvErrors)
        {
            live.mvErrors = UavBuffer(g.device, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (auto& r : live.mvErrorsReadback)
                r = MakeBuffer(g.device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (live.mvErrors)
        {
            Transition(list.Get(), live.mvErrors.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.mvErrors.Get(), 0, g.zeros.Get(), 0, 256);
            Transition(list.Get(), live.mvErrors.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetComputeRootSignature(live.mvcalRs.Get());
            list->SetPipelineState(live.mvcalPso.Get());
            list->SetComputeRootShaderResourceView(0, live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.depthPrevBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, live.mvBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, live.mvErrors->GetGPUVirtualAddress());
            const uint32_t mc[8] = { live.width, live.height, live.depthPitch, live.mvPitch, d24 ? 1u : 0u, 0, 0, 0 };
            list->SetComputeRoot32BitConstants(5, 8, mc, 0);
            list->Dispatch(16, 1, 1); // 4096 samples
            Transition(list.Get(), live.mvErrors.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.mvErrorsReadback[live.frame % 3].Get(), 0, live.mvErrors.Get(), 0, 256);
            Transition(list.Get(), live.mvErrors.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    if (motionOk)
    {
        Transition(list.Get(), live.mvBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(list.Get(), live.depthPrevBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (live.prevShadowBuf)
        Transition(list.Get(), live.prevShadowBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    if (atlasValid)
        Transition(list.Get(), live.atlasBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(live.sunReadback[live.frame % 3].Get(), 0, live.sunBuf.Get(), 0, kLiveSunBytes);
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // camera orientation (world normals of the G-buffer): solved here, read by this frame's reflections
    if (gnRotNow && live.gnStats)
    {
        D3D12_RESOURCE_BARRIER rb[2] = {};
        rb[0].Type = rb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        rb[0].UAV.pResource = live.gnStats.Get(), rb[1].UAV.pResource = live.sunBuf.Get();
        list->ResourceBarrier(2, rb);
        list->SetComputeRootSignature(live.rotfitRs.Get());
        list->SetPipelineState(live.rotfitPso.Get());
        list->SetComputeRootUnorderedAccessView(0, live.gnStats->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, live.sunBuf->GetGPUVirtualAddress());
        const float minAgree = 0.35f; // (texture relief in the game's normals: 0.5 to 0.65 for the right rotation, close to 0 for a wrong one)
        uint32_t rc[4] = { 300u, 0, 0, 0 };
        std::memcpy(&rc[1], &minAgree, 4);
        list->SetComputeRoot32BitConstants(2, 4, rc, 0);
        list->Dispatch(1, 1, 1);
        list->ResourceBarrier(1, &rb[1]);
    }

    LiveTime(list.Get(), 4);
    LiveTimeD(list.Get(), 11);
    // reflections: no more settings in the menu (strength 1, no wet floor); urti.ini still rules for tests
    {
        int32_t view = live.iniDebugView;
        if (view < 0)
        {
            view = 0;
            if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiDebug"); v.handle)
                runtime->get_uniform_value_int(v, &view, 1);
        }
        if (uint32_t(view) != live.debugView)
            Log("URTI live: diagnostic view %d", view);
        live.debugView = uint32_t(std::clamp(view, 0, 11));
    }
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiGenerated"); v.handle)
    {
        int32_t m = 0;
        runtime->get_uniform_value_int(v, &m, 1);
        live.fgMode = uint32_t(std::clamp(m, 0, 2));
    }
    live.reflEffective = live.menuRefl ? live.reflStrength : 0.0f;
    const float smoothMinEffective = live.smoothMin;
    // 4b. reflections: one reflected ray per smooth pixel (G-buffer material copied in this frame)
    if (live.reflEffective > 0 || live.debugReflect)
    {
        const uint64_t bytes = uint64_t(live.width) * live.height * 16;
        if (!live.reflectBuf || live.reflectBytes < bytes)
        {
            Retire(live.reflectBuf);
            live.reflectBuf = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.reflectBytes = bytes;
        }
        // this frame's material (ring, moves from one queue to the other without a barrier), or the single copy without frame_sync
        ID3D12Resource* materialNow = live.frameSync ? (live.materialIndexNow >= 0 ? live.materialSnap[live.materialIndexNow].Get() : nullptr) : live.materialBuf.Get();
        const bool materialRing = live.frameSync;
        const bool material = materialNow && live.materialFrame != ~0ull && live.frame <= live.materialFrame + 8 &&
                              live.materialWidth >= live.width && live.materialHeight >= live.height;
        if (live.anchorBytes < bytes)
        {
            for (auto& a : live.anchor)
                Retire(a), a = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.anchorBytes = bytes;
            live.anchorOut = -1;
        }
        if (live.reflectBuf && live.reflectPso && live.anchor[0] && live.anchor[1])
        {
            // this frame's anchoring: the other buffer keeps the previous one's
            live.anchorOut = live.anchorOut < 0 ? 0 : 1 - live.anchorOut;
            live.anchorPrevOk = live.anchorFrame != ~0ull && live.anchorFrame + 1 == live.frame;
            live.anchorFrame = live.frame;
            if (materialNow && !materialRing)
                Transition(list.Get(), materialNow, D3D12_RESOURCE_STATE_COPY_DEST, srv);
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            // surface cache: read only if it covers the whole scene and has already been filled
            const bool surfRead = live.surfCacheOn && live.surfCache && live.surfFilled > 0 && live.surfTriangles >= live.capacity / 3;
            if (surfRead)
                Transition(list.Get(), live.surfCache.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.reflectRs.Get());
            list->SetPipelineState(live.reflectPso.Get());
            list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, materialNow ? materialNow->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(4, sceneTable->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(5, linksBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(6, live.view->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(7, live.viewPrev ? live.viewPrev->GetGPUVirtualAddress() : live.view->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(8, surfRead ? live.surfCache->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(9, live.transpBuilt ? live.viewT->GetGPUVirtualAddress() : live.view->GetGPUVirtualAddress());
            const bool geoColorsRead = live.geoColors && live.geoOn;
            if (geoColorsRead)
                Transition(list.Get(), live.geoColors.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            // world radiance cache: only with the game's camera (exact world frame)
            bool radNow = live.radOn && live.radPso && trueCam && live.worldUpKnown && live.camHaveCur;
            if (radNow)
            {
                const uint64_t hashBytes = (16ull << live.radBits), envBytes = uint64_t(live.radEnvSize) * live.radEnvSize * 16;
                if (!live.radHash)
                {
                    live.radHash = UavBuffer(g.device, hashBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // zeroed: empty
                    live.radEnv = UavBuffer(g.device, envBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    live.radStats = UavBuffer(g.device, 64, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    for (auto& r : live.radStatsRb)
                        r = MakeBuffer(g.device, 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                    QueryPerformanceCounter(&live.radClock0);
                    Log("URTI live: world radiance cache: %u cells (%.0f MB), sky map %u × %u", 1u << live.radBits, hashBytes / 1048576.0, live.radEnvSize, live.radEnvSize);
                }
                if (!live.reflMix || live.reflMixBytes < bytes)
                {
                    Retire(live.reflMix);
                    live.reflMix = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    live.reflMixBytes = live.reflMix ? bytes : 0;
                }
                radNow = live.radHash && live.radEnv && live.radStats && live.reflMix;
            }
            ID3D12Resource* radParams = nullptr;
            if (radNow)
            {
                const GameCamera& gc = live.camCur;
                LARGE_INTEGER now, freq;
                QueryPerformanceCounter(&now), QueryPerformanceFrequency(&freq);
                const float d0 = 4.0f;
                const struct
                {
                    float right[3], shearX, up[3], shearY, fwd[3], voxel0, pos[3], d0;
                    uint32_t mask, envSize, upAxis;
                    float upSign;
                    uint32_t tick, frame;
                    float envFar;
                    uint32_t pad;
                } rp = { { gc.right[0], gc.right[1], gc.right[2] }, live.camShear[0], { gc.up[0], gc.up[1], gc.up[2] }, live.camShear[1],
                         { -gc.back[0], -gc.back[1], -gc.back[2] }, d0 * live.radVoxel, { gc.pos[0], gc.pos[1], gc.pos[2] }, d0,
                         (1u << live.radBits) - 1u, live.radEnvSize, uint32_t(live.worldUpAxis), live.worldUpSign,
                         uint32_t((now.QuadPart - live.radClock0.QuadPart) * 4 / std::max<LONGLONG>(freq.QuadPart, 1)) + 64u, uint32_t(live.frame), 400.0f, 0 };
                radParams = upload(live.radUpload[live.frame % 3], &rp, sizeof(rp));
                radNow = radParams != nullptr;
            }
            if (radNow)
            {
                Transition(list.Get(), live.radHash.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                Transition(list.Get(), live.radEnv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            }
            live.reflMixValid = radNow, live.radParamsNow = radParams, live.radStamp = radNow ? live.frame : ~0ull;
            // counters (reflect_stats=1): reset, read three frames later
            const bool reflStatsNow = live.reflStatsOn;
            if (reflStatsNow && !live.reflStats)
            {
                live.reflStats = UavBuffer(g.device, 64, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                for (auto& r : live.reflStatsRb)
                    r = MakeBuffer(g.device, 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            if (reflStatsNow)
            {
                if (const uint32_t* v = nullptr; live.reflStatsRb[live.frame % 3] && SUCCEEDED(live.reflStatsRb[live.frame % 3]->Map(0, nullptr, (void**)&v)) && v)
                {
                    for (int k = 0; k < 8; k++)
                        live.statRs[k] += v[k];
                    live.reflStatsRb[live.frame % 3]->Unmap(0, nullptr);
                    if (++live.statRsFrames == 300)
                    {
                        const double f = 300.0, screen = double(live.width) * live.height;
                        Log("URTI live: reflections per frame: %.1f %% of the screen reflecting, %.1f %% of it water; %.0f rays, %.0f pixels skipped, %.0f rays stopped at the hop cap; level %d; "
                            "world cache: %.0f colours read, %.0f ray remainders taken from the sky map, %.0f pixels without colour",
                            live.statRs[0] / f / screen * 100.0, live.statRs[1] / f / screen * 100.0, live.statRs[2] / f, live.statRs[3] / f, live.statRs[4] / f, live.reflLevel,
                            live.statRs[5] / f, live.statRs[6] / f, live.statRs[7] / f);
                        for (auto& v2 : live.statRs)
                            v2 = 0;
                        live.statRsFrames = 0;
                    }
                }
                Transition(list.Get(), live.reflStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyBufferRegion(live.reflStats.Get(), 0, g.zeros.Get(), 0, 32);
                Transition(list.Get(), live.reflStats.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            list->SetComputeRootShaderResourceView(10, geoColorsRead ? live.geoColors->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(11, radNow ? live.radHash->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(12, radNow ? live.radEnv->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(13, radNow ? radParams->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            const bool gnRefl = live.gnNowFrame != nullptr; // (bit 5 of gnInfo: the reflections' normal takes them, gnormal_reflect=1)
            list->SetComputeRootShaderResourceView(14, gnRefl ? live.gnNowFrame->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(15, live.reflectBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(16, live.anchor[live.anchorOut]->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(18, radNow ? live.reflMix->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
            const struct
            {
                uint32_t width, height, depthPitch, materialPitch;
                float A, B, p00, p11, clearDepth;
                uint32_t d24, channel;
                float smoothMin, bias;
                uint32_t flags;
                float f0;
                uint32_t reflectPitch, regionCount, anchors, prevVertexCount, cacheTriangles;
                float up[3];
                uint32_t budget;
                float tMaxNow;
                uint32_t gnInfo;
            } rc = { live.width, live.height, live.depthPitch, live.materialPitch, live.A, live.B, live.p00, live.p11, g.depthReversed ? 0.0f : 1.0f,
                     d24 ? 1u : 0u, live.reflChannel, smoothMinEffective, 0.002f, (material ? 1u : 0u) | (live.reflInvert ? 2u : 0u) | (live.reflEverywhere ? 4u : 0u) | (live.debugView == 7 ? 8u : 0u) |
                     (live.debugView == 4 || live.debugReflect == 1 ? 16u : 0u) | (live.alphaScreen ? 32u : 0u) | (surfRead ? 64u : 0u) |
                         (live.transpBuilt ? 256u : 0u) | (live.reflectDiag ? 256u << live.reflectDiag : 0u) | (trueCam && live.worldUpKnown && live.reflectUp ? 8192u : 0u) | (geoColorsRead ? 16384u : 0u) | (live.slowRays ? 32768u : 0u) |
                         (trueCam && live.worldUpKnown && live.reflectUp && live.waterOnly ? 65536u : 0u) | (reflStatsNow ? 131072u : 0u) | (radNow ? 262144u : 0u) | (live.reflCoherent ? 524288u : 0u), live.f0, live.width * 16,
                     uint32_t(tableNow.size()), live.anchorAlpha > 0 && live.viewPrev && live.prevValid && !radNow ? 1u : 0u, uint32_t(live.capacity),
                     surfRead ? uint32_t(live.surfTriangles) : 0u, { live.worldUp[0], live.worldUp[1], live.worldUp[2] },
                     live.reflStrideNow | (live.reflHopsNow << 4) | (uint32_t(live.frame) << 8), live.reflTMaxNow,
                     (gnRefl ? (live.gnMode & 3) | (live.gnSigns << 2) | (live.gnReflect ? 32u : 0u) | (live.gnPitch << 16) : 0u) |
                         (live.menuGloss && live.glossMin > 0 && live.glossAmount > 0
                              ? (uint32_t(std::clamp(live.glossMin, 0.0f, 1.0f) * 127.0f + 0.5f) << 6) | (uint32_t(std::clamp(live.glossAmount, 0.0f, 1.0f) * 7.0f + 0.5f) << 13)
                              : 0u) };
            list->SetComputeRootUnorderedAccessView(17, reflStatsNow ? live.reflStats->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstants(19, 26, &rc, 0);
            list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
            if (reflStatsNow && live.reflStatsRb[live.frame % 3])
            {
                Transition(list.Get(), live.reflStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyBufferRegion(live.reflStatsRb[live.frame % 3].Get(), 0, live.reflStats.Get(), 0, 32);
                Transition(list.Get(), live.reflStats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (surfRead)
                Transition(list.Get(), live.surfCache.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (geoColorsRead)
                Transition(list.Get(), live.geoColors.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (radNow)
            {
                Transition(list.Get(), live.radHash.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Transition(list.Get(), live.radEnv.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                D3D12_RESOURCE_BARRIER mb = {};
                mb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                mb.UAV.pResource = live.reflMix.Get();
                list->ResourceBarrier(1, &mb);
            }
            // this frame's scene: the cache will be filled at composition, on the presented image
            live.sceneTable = sceneTable, live.sceneCount = uint32_t(tableNow.size()), live.sceneStamp = live.frame;
            Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (materialNow && !materialRing)
                Transition(list.Get(), materialNow, srv, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER uavb = {};
            uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavb.UAV.pResource = live.reflectBuf.Get();
            list->ResourceBarrier(1, &uavb);
        }
    }
    // G-buffer normals (without a ring): given back at the next frame's copy
    if (live.gnNowFrame && !live.frameSync)
        Transition(list.Get(), live.gnNowFrame, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    live.gnNowFrame = nullptr;

    LiveTime(list.Get(), 5);
    if (live.timeDHeap && live.timeDRb[live.frame % 3])
        list->ResolveQueryData(live.timeDHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (live.frame % 3) * LiveState::kTimeD, 12, live.timeDRb[live.frame % 3].Get(), 0);
    if (live.timeHeap && live.timeReadback[live.frame % 3])
        list->ResolveQueryData(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (live.frame % 3) * 8, 6, live.timeReadback[live.frame % 3].Get(), 0);
    // 5. effect layers for the composition
    Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list.Get(), live.mask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = live.mask.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = live.shadowBuf.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, live.width, live.height, 1, live.shadowPitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(list.Get(), live.mask.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // this frame's vertices kept for the next (geometric memory)
    if (live.viewPrev && live.anchorAlpha > 0)
    {
        Transition(list.Get(), live.view.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list.Get(), live.viewPrev.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.viewPrev.Get(), 0, live.view.Get(), 0, std::min<uint64_t>(cursorNow / 16, live.capacity) * 12);
        Transition(list.Get(), live.viewPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
        Transition(list.Get(), live.view.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.prevStart.swap(live.pendingStart);
        live.prevValid = true;
    }
    else
    {
        Transition(list.Get(), live.view.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.prevValid = false;
    }
    if (live.frameTrace)
    {
        const uint32_t s = uint32_t(live.frame % 3), y = live.height * 6 / 10, bytes = live.width * 4;
        if (!live.traceRb[s] || live.traceRb[s]->GetDesc().Width < bytes * 2ull)
            Retire(live.traceRb[s]), live.traceRb[s] = MakeBuffer(g.device, bytes * 2ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (live.traceRb[s])
        {
            Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.traceRb[s].Get(), 0, live.depthBuf.Get(), uint64_t(y) * live.depthPitch, bytes);
            Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, srv);
            Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.traceRb[s].Get(), bytes, live.shadowBuf.Get(), uint64_t(y) * live.shadowPitch, bytes);
            Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.traceFrame[s] = live.frame, live.traceWidth[s] = live.width;
        }
    }
    Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);

    // 6. counters: read later, then reset for the next frame
    Transition(list.Get(), g.counters.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(live.counterReadback[live.frame % 3].Get(), 0, g.counters.Get(), 0, kSlots * 8ull);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    zeroCounters(D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
    for (uint32_t s = 0; s < kSlots; s++)
    {
        g.slotScore[s] = 0.9f * g.slotScore[s] + float(g.slotDraws[s]);
        g.slotDraws[s] = 0;
    }
    if (slot != live.lastMainSlot)
    {
        const D3D12_RESOURCE_DESC md = slot >= 0 && g.slotDepth[slot] ? reinterpret_cast<ID3D12Resource*>(g.slotDepth[slot])->GetDesc() : D3D12_RESOURCE_DESC{};
        Log("URTI live: main screen: stream %d, depth %llux%u (%u draws in this frame)", slot, (unsigned long long)md.Width, md.Height, drawsThisFrame);
        live.lastMainSlot = slot;
        LiveGeoFlush("another main screen");
    }
    live.lastDrawsThisFrame = drawsThisFrame;

    if (live.maskFrame != ~0ull)
        live.statGapMax = std::max(live.statGapMax, uint32_t(live.frame - live.maskFrame));
    live.maskFrame = live.frame;
    if (active.handle)
        runtime->set_uniform_value_bool(active, true);
    const effect_uniform_variable showMask = runtime->find_uniform_variable("urti.fx", "UrtiShowMask");
    // urti.ini show_mask=1 ticks "mask only" in the effect; removed, it unticks it (otherwise the view stays)
    if (showMask.handle && g_liveShowMask != live.iniMaskSet)
        runtime->set_uniform_value_bool(showMask, g_liveShowMask), live.iniMaskSet = g_liveShowMask;
    if (!live.bound && active.handle)
    {
        live.bound = true;
        live.boundFrame = live.frame;
        Log("URTI live: first frame: stream %d, %u draws, %llu vertices filled (structure %llu), depth %ux%u format %d, uniform %s",
            slot, drawsThisFrame, (unsigned long long)live.lastFilled, (unsigned long long)live.capacity, live.width, live.height,
            (int)depthDesc.Format, active.handle ? "found" : "MISSING");
    }
    live.frame++;
}

// RT effects laid on the presented image, on ReShade's immediate list (executed before the
// present). Every present comes through here, including frames generated by frame
// generation (DLSS-G / FSR), which are built from the game's image before ReShade: without this,
// every other frame would lack the RT effects.
void LiveComposite(effect_runtime* runtime)
{
    live.composedNow = false;
    if (live.showGameOnly)
        return; // F4: before (the RT computation goes on: going back to "after" is immediate)
    if (!live.enabled || !live.depthReady || !live.bound || !live.shadowBuf || !live.compositePso)
        return;
    // mask of a frame too old (computation skipped: calibration, missing depth…): nothing rather than frozen
    // shadows that no longer follow the camera. Counted generously: with frame generation, several presents in
    // a row without a game draw arrive in normal operation ("00001"); the effect cut on one of them showed
    // as a blink
    if (live.maskFrame == ~0ull || live.frame > live.maskFrame + 8)
    {
        live.statMaskOld++;
        return;
    }
    if (live.overlayOpen)
    {
        live.statSkipOverlay++;
        return; // ReShade menu open: it must stay readable (the pass would cover it)
    }
    if (live.fgMode == 1 && live.imageGenerated)
        return; // generated frame left as it is (menu setting)
    const effect_technique technique = runtime->find_technique("urti.fx", "URTI");
    const bool techniqueOn = runtime->get_effects_state() && (!technique.handle || runtime->get_technique_state(technique));
    if (techniqueOn != live.lastTechnique)
        Log("URTI live: %s in ReShade", techniqueOn ? "URTI effect on" : "URTI effect off (or effects off): no RT effects");
    live.lastTechnique = techniqueOn;
    if (!techniqueOn)
    {
        live.statSkipTechnique++;
        return;
    }
    const resource back = runtime->get_current_back_buffer();
    if (!back.handle)
        return;
    ID3D12Resource* bb = reinterpret_cast<ID3D12Resource*>(back.handle);
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    uint32_t format = 0;
    switch (bd.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: format = 0; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: format = 1; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: format = 2; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: format = 3; break;
    default: return;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    if (!live.imageIn || live.imageBytes < total)
    {
        Retire(live.imageIn), Retire(live.imageOut);
        live.imageIn = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
        live.imageOut = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.imageBytes = total;
        if (!live.imageIn || !live.imageOut)
            return;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(reinterpret_cast<IUnknown*>(runtime->get_command_queue()->get_immediate_command_list()->get_native())->QueryInterface(IID_PPV_ARGS(&list))))
        return;
    // screen snapshots before the UI: buffers (same layout as imageIn), known screen images,
    // choice from the comparisons read three presents later
    // (and a new layout as soon as the screen image changes format or size: switch to HDR, Alt+Tab)
    const bool snapLayoutChanged = g_liveSnapFp.Footprint.Format != fp.Footprint.Format || g_liveSnapFp.Footprint.Width != fp.Footprint.Width ||
                                   g_liveSnapFp.Footprint.Height != fp.Footprint.Height || g_liveSnapFp.Footprint.RowPitch != fp.Footprint.RowPitch;
    if (live.snapBytes < total || snapLayoutChanged)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (live.snapBytes < total)
        {
            for (uint32_t k = 0; k < kLiveSnaps; k++)
            {
                Retire(live.snaps[k]);
                live.snaps[k] = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
                g_liveSnap[k] = live.snaps[k].Get();
            }
            Retire(live.preUiBuf);
            live.preUiBuf = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.snapBytes = total;
        }
        if (snapLayoutChanged && g_liveSnapFp.Footprint.Width)
            Log("URTI live: screen image changed (format %d, %ux%u): snapshots before the UI reset", int(fp.Footprint.Format),
                fp.Footprint.Width, fp.Footprint.Height);
        g_liveSnapFp = fp;
        live.uiChoice = -1;
    }
    {
        // screen images of this present only: after a recreation, the old ones are destroyed and
        // their address may serve another game image
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveBackBuffers.clear();
        for (uint32_t i = 0; i < runtime->get_back_buffer_count(); i++)
            g_liveBackBuffers.insert(runtime->get_back_buffer(i).handle);
    }
    if (!live.uiCounts)
    {
        live.uiCounts = UavBuffer(g.device, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto& r : live.uiReadback)
            r = MakeBuffer(g.device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    const uint32_t uiSlot = uint32_t(live.uiFrame % 3);
    if (const uint32_t* c = nullptr; live.uiFrame >= 3 && live.uiCountAt[uiSlot] && SUCCEEDED(live.uiReadback[uiSlot]->Map(0, nullptr, (void**)&c)) && c)
    {
        // choice made by live_uiselect on the GPU, in the same frame (bytes 64 and 68 depending on parity)
        const int choice = int(c[16 + live.uiParity[uiSlot]]) - 1;
        const float best = choice >= 0 && choice < 4 && c[8] ? float(c[choice]) / float(c[8]) : 0.0f;
        if (choice != live.uiChoice)
        {
            char detail[160] = "";
            for (uint32_t k = 0, n = 0; k < live.uiCountAt[uiSlot] && k < 4 && c[8]; k++)
                n += std::snprintf(detail + n, sizeof(detail) - n, " ; photo %u : %.1f %% / %.1f %%", k, 100.0 * c[k] / c[8], 100.0 * c[4 + k] / c[8]);
            Log(choice >= 0 ? "URTI live: image before the game's UI: snapshot %d (%.1f %% of pixels identical to the final image)%s"
                            : "URTI live: no snapshot before the game's UI (%d, %.1f)%s (identical to the final image / to the previous one)",
                choice, best * 100.0f, detail);
        }
        live.uiChoice = choice;
        live.uiReadback[uiSlot]->Unmap(0, nullptr);
    }
    uint32_t snapCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snapCount = std::min(g_liveSnapCount, kLiveSnaps);
    }
    // effect settings: split screen (F5), mask only (check); no more strength in the menu: the
    // sunlight is removed entirely, as in the game's shadows
    const float strength = 1.0f;
    bool split = false, showMask = false;
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiSplit"); v.handle)
        runtime->get_uniform_value_bool(v, &split, 1);
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiShowMask"); v.handle)
        runtime->get_uniform_value_bool(v, &showMask, 1);
    showMask = showMask || g_liveShowMask || (live.debugView >= 1 && live.debugView <= 3);
    if (strength != live.lastStrength || split != live.lastSplit || showMask != live.lastShowMask)
        Log("URTI live: menu settings: strength %.2f, split screen %d, mask only %d", strength, split, showMask);
    live.lastStrength = strength, live.lastSplit = split, live.lastShowMask = showMask;

    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_TEXTURE_COPY_LOCATION tex = {}, buf = {};
    tex.pResource = bb;
    tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buf.PlacedFootprint = fp;
    const uint32_t slotTime = uint32_t((live.frame + 2) % 3); // frame whose effects were just recorded
    if (live.timeHeap)
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 6);
    live.compCounter++;
    // detailed composition of the present from three ago: UI, reflections (stabilisation, surface cache), light, composition, copy
    if (const uint64_t* t = nullptr; live.timeCRb[live.compCounter % 3] && SUCCEEDED(live.timeCRb[live.compCounter % 3]->Map(0, nullptr, (void**)&t)) && t)
    {
        UINT64 freq = 0;
        if (ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()); q && SUCCEEDED(q->GetTimestampFrequency(&freq)) && freq &&
            t[5] > t[0] && t[5] - t[0] < freq)
        {
            for (int k = 0; k < 5; k++)
                live.timeDSum[12 + k] += double(t[k + 1] - t[k]) * 1000.0 / double(freq);
            if (++live.timeCFrames == 300)
            {
                const double n = live.timeCFrames;
                Log("URTI live: GPU detail (ms per present): UI-free image %.2f, stabilised reflections and surface cache %.2f, light %.2f, "
                    "composition %.2f, copie %.2f", live.timeDSum[12] / n, live.timeDSum[13] / n, live.timeDSum[14] / n, live.timeDSum[15] / n, live.timeDSum[16] / n);
                for (int k = 12; k < 17; k++)
                    live.timeDSum[k] = 0;
                live.timeCFrames = 0;
            }
        }
        live.timeCRb[live.compCounter % 3]->Unmap(0, nullptr);
    }
    LiveTimeD(list.Get(), 12);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    buf.pResource = live.imageIn.Get();
    list->CopyTextureRegion(&buf, 0, 0, 0, &tex, nullptr);
    Transition(list.Get(), live.imageIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    for (uint32_t k = 0; k < snapCount; k++)
        Transition(list.Get(), live.snaps[k].Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    // image before the UI: this frame's snapshots compared with the final image and the previous displayed
    // image, choice and copy on the GPU (live_uicompare then live_uiselect), in the same frame
    live.uiCountAt[uiSlot] = 0;
    ID3D12Resource* preUi = nullptr;
    bool hudLead = false; // live_hudgain wrote the layer's lead on this present's image
    // the game's image without its UI, copied inside its lists: adopted from the execution votes, then taken as it is (no
    // snapshot or comparison) as long as a recent copy exists
    int32_t hudSlot = -1, hudPrevSlot = -1, hudPrev2Slot = -1;
    uint32_t hudPitch = 0;
    uint64_t hudSerialNow = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveHudW = live.hudlessOn && live.frameSync && (format == 2 || format == 3) ? uint32_t(bd.Width) : 0u, g_liveHudH = bd.Height;
        if (g_liveHudVoteCount >= 40)
        {
            uint64_t best = 0;
            uint32_t votes = 0;
            for (const auto& [h, n] : g_liveHudVotes)
                if (n > votes)
                    best = h, votes = n;
            if (votes * 10 >= g_liveHudVoteCount * 8 && best != g_liveHudHandle)
                Log("URTI live: the game's UI-free image recognised (%u times out of %u, %zu candidates, screen image format %d): reflection colours "
                    "taken before the HUD, UI left intact", votes, g_liveHudVoteCount, g_liveHudVotes.size(), int(bd.Format)),
                    g_liveHudHandle = best, g_liveHudDone = -1;
            else if (g_liveHudHandle && g_liveHudVotes[g_liveHudHandle] * 2 < g_liveHudVoteCount)
                Log("URTI live: UI-free image no longer recognised (%u times out of %u)", g_liveHudVotes[g_liveHudHandle], g_liveHudVoteCount),
                    g_liveHudHandle = 0, g_liveHudDone = -1;
            g_liveHudVotes.clear(), g_liveHudVoteCount = 0;
        }
        const uint64_t presentNow = g_presentIndex.load(std::memory_order_relaxed);
        if (live.hudlessOn && g_liveHudHandle && g_liveHudDone >= 0 && presentNow - g_liveHudDonePresent <= 8 && live.hudSnap[g_liveHudDone] &&
            live.hudWidth == uint32_t(bd.Width) && live.hudHeight == bd.Height && live.hudPitch * uint64_t(bd.Height) <= live.hudBytes)
        {
            hudSlot = g_liveHudDone, hudPitch = live.hudPitch, hudSerialNow = g_liveHudSerial;
            // previous copy (the ring is filled in order): still pixels
            const int32_t prev = int32_t((uint32_t(hudSlot) + LiveState::kHudSnaps - 1) % LiveState::kHudSnaps);
            if (g_liveHudNext >= 2 && live.hudSnap[prev])
                hudPrevSlot = prev;
            const int32_t prev2 = int32_t((uint32_t(hudSlot) + LiveState::kHudSnaps - 2) % LiveState::kHudSnaps);
            if (hudPrevSlot >= 0 && g_liveHudNext >= 3 && live.hudSnap[prev2])
                hudPrev2Slot = prev2;
        }
    }
    if (hudSlot >= 0 && live.hudPso && live.preUiBuf && live.uiCounts)
    {
        if (!live.hudGain)
        {
            live.hudGain = UavBuffer(g.device, 2048, D3D12_RESOURCE_STATE_COPY_DEST);
            if (live.hudGain)
            {
                list->CopyBufferRegion(live.hudGain.Get(), 0, g.zeros.Get(), 0, 16 + 256 * 4 + 64);
                Transition(list.Get(), live.hudGain.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            for (auto& r : live.hudGainReadback)
                r = MakeBuffer(g.device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (const uint64_t maskBytes = uint64_t(bd.Width) * bd.Height * 4; !live.uiMaskBuf || live.uiMaskBytes < maskBytes)
            Retire(live.uiMaskBuf), live.uiMaskBuf = UavBuffer(g.device, maskBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), live.uiMaskBytes = maskBytes;
    }
    if (hudSlot < 0)
        live.hudConverted = ~0ull;
    // on every present: conversion if the copy is new; then one pixel in 64 compared with the final image (does this present show
    // the copy's image? with DLSS-G, one in two is made between two game frames); if so, UI mask of every pixel
    if (hudSlot >= 0 && live.hudPso && live.hudGainPso && live.preUiBuf && live.uiCounts && live.uiMaskBuf && live.hudGain)
    {
        // the last three copies: a present's final image is that of an older copy (the game is ahead)
        const uint32_t copies = hudPrev2Slot >= 0 ? 3u : hudPrevSlot >= 0 ? 2u : 1u;
        // converted copy: the one of the last computation's game frame (its colours go with its depth), if it is still among the last three
        const uint64_t convSerial = live.layerHudSerial <= hudSerialNow && hudSerialNow - live.layerHudSerial < copies ? live.layerHudSerial : hudSerialNow;
        const uint32_t convBack = uint32_t(hudSerialNow - convSerial);
        live.statHudConvBack[std::min<uint64_t>(live.layerHudSerial > hudSerialNow ? 3 : hudSerialNow - live.layerHudSerial, 3)]++;
        const bool fresh = convSerial != live.hudConverted;
        const int32_t convSlot = convBack == 0 ? hudSlot : convBack == 1 ? hudPrevSlot : hudPrev2Slot;
        if (fresh)
        {
            live.hudConverted = convSerial, live.statHudConverted++;
            // no snapshot choice (bytes 64 and 68 zeroed): live_composite takes the UI mask written here and the final image's colours
            Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.uiCounts.Get(), 0, g.zeros.Get(), 0, 72);
            Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        struct
        {
            uint32_t width, height, srcPitch, dstPitch, format;
            float alphaUi;
            uint32_t mode;
            float diffUi;
        } hc = { uint32_t(bd.Width), bd.Height, hudPitch, fp.Footprint.RowPitch, format, 0.02f, 1u, 0.025f };
        const auto bindHud = [&]() {
            list->SetComputeRootSignature(live.hudRs.Get());
            list->SetPipelineState(live.hudPso.Get());
            list->SetComputeRootShaderResourceView(0, live.hudSnap[hudSlot]->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.hudSnap[hudPrevSlot >= 0 ? hudPrevSlot : hudSlot]->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, live.imageIn->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, live.hudSnap[hudPrev2Slot >= 0 ? hudPrev2Slot : hudSlot]->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, live.preUiBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(5, live.uiMaskBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(6, live.hudGain->GetGPUVirtualAddress());
        };
        bindHud();
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        if (fresh)
        {
            list->SetComputeRootShaderResourceView(0, live.hudSnap[convSlot >= 0 ? convSlot : hudSlot]->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstants(7, 8, &hc, 0);
            list->Dispatch((hc.width + 7) / 8, (hc.height + 7) / 8, 1);
            list->SetComputeRootShaderResourceView(0, live.hudSnap[hudSlot]->GetGPUVirtualAddress());
        }
        hc.mode = 2u | ((copies - 1) << 8);
        list->SetComputeRoot32BitConstants(7, 8, &hc, 0);
        list->Dispatch(((hc.width + 7) / 8 + 7) / 8, ((hc.height + 7) / 8 + 7) / 8, 1);
        // ratio: running average of the medians (at least 1000 pixels), and share of matching pixels; read for the log
        ub.UAV.pResource = live.hudGain.Get();
        list->ResourceBarrier(1, &ub);
        const float blendNow = 0.1f;
        uint32_t gc[8] = { 1000u, 0, uint32_t(hudSerialNow), uint32_t(g_presentIndex.load(std::memory_order_relaxed)), uint32_t(live.layerHudSerial), 0, 0, 0 };
        std::memcpy(&gc[1], &blendNow, 4);
        std::memcpy(&gc[5], &live.lagBias, 4);
        list->SetComputeRootSignature(live.hudGainRs.Get());
        list->SetPipelineState(live.hudGainPso.Get());
        list->SetComputeRootUnorderedAccessView(0, live.hudGain->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(1, 8, gc, 0);
        list->Dispatch(1, 1, 1);
        list->ResourceBarrier(1, &ub);
        hudLead = live.lagOn && format != 3;
        if (format != 3 && (live.uiFrame & 3) == 0)
        {
            hc.mode = 4u | ((copies - 1) << 8);
            bindHud();
            list->SetComputeRoot32BitConstants(7, 8, &hc, 0);
            list->Dispatch((hc.width + 7) / 8, (hc.height + 7) / 8, 1);
        }
        ub.UAV.pResource = live.uiMaskBuf.Get();
        list->ResourceBarrier(1, &ub);
        if (const uint32_t rs = uint32_t(live.uiFrame % 3); live.hudGainReadback[rs])
        {
            // (read three presents later: matching presents counted for the log)
            if (const float* gv = nullptr; SUCCEEDED(live.hudGainReadback[rs]->Map(0, nullptr, (void**)&gv)) && gv)
            {
                const uint32_t lag = std::min(((const uint32_t*)gv)[4], 2u);
                live.statLeadSum += gv[5];
                live.statHudMatched += gv[2] >= 0.96f ? 1u : 0u, live.statHudMatchSum += gv[2];
                if (gv[2] >= 0.96f)
                    live.statHudLag[lag]++;
                live.hudSlopeNow = gv[3];
                live.hudGainReadback[rs]->Unmap(0, nullptr);
            }
            Transition(list.Get(), live.hudGain.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.hudGainReadback[rs].Get(), 0, live.hudGain.Get(), 0, 16);
            list->CopyBufferRegion(live.hudGainReadback[rs].Get(), 16, live.hudGain.Get(), 16 + 256 * 4 + 32, 4);
            list->CopyBufferRegion(live.hudGainReadback[rs].Get(), 20, live.hudGain.Get(), 16 + 256 * 4 + 44, 4);
            Transition(list.Get(), live.hudGain.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (live.hudlessMode != 2)
        {
            Transition(list.Get(), live.preUiBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            preUi = live.preUiBuf.Get();
        }
        live.uiChoice = -1, live.uiBadReadbacks = 0;
        live.statHudless++;
    }
    else if (snapCount && live.uiPhoto && live.uiPso && live.uiSelPso && live.preUiBuf)
    {
        list->CopyBufferRegion(live.uiCounts.Get(), 0, g.zeros.Get(), 0, 64);
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = live.uiCounts.Get();
        list->ResourceBarrier(1, &ub);
        Transition(list.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        const uint32_t parity = uint32_t(live.uiFrame & 1);
        live.uiParity[uiSlot] = parity;
        const uint32_t uc[8] = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format == 3 ? 8u : 4u, snapCount, parity, 0, 0 };
        list->SetComputeRootSignature(live.uiRs.Get());
        list->SetPipelineState(live.uiPso.Get());
        list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
        for (uint32_t k = 0; k < kLiveSnaps; k++)
            list->SetComputeRootShaderResourceView(1 + k, live.snaps[std::min(k, snapCount - 1)]->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(5, live.imageOut->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(6, live.uiCounts->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(7, 8, uc, 0);
        list->Dispatch((uint32_t(bd.Width) / 4 + 7) / 8, (bd.Height / 4 + 7) / 8, 1);
        Transition(list.Get(), live.imageOut.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &ub); // counters written, read (and choice kept) by live_uiselect
        list->SetComputeRootSignature(live.uiSelRs.Get());
        list->SetPipelineState(live.uiSelPso.Get());
        list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
        for (uint32_t k = 0; k < kLiveSnaps; k++)
            list->SetComputeRootShaderResourceView(1 + k, live.snaps[std::min(k, snapCount - 1)]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(5, live.preUiBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(6, live.uiCounts->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(7, 8, uc, 0);
        list->Dispatch((uint32_t(bd.Width) + 7) / 8, (bd.Height + 7) / 8, 1);
        Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.uiReadback[uiSlot].Get(), 0, live.uiCounts.Get(), 0, 72);
        Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list.Get(), live.preUiBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        live.uiCountAt[uiSlot] = snapCount;
        preUi = live.preUiBuf.Get();
    }
    live.uiFrame++;
    LiveTimeD(list.Get(), 13);
    // no safe snapshot before the UI (read three frames later): the reflections' colours would come from
    // the final image, menus included; RT reflections fade out smoothly (the game keeps its own) and come back
    // with the snapshot
    // (an occasional stale snapshot is caught by live_composite: last safe snapshot; only
    // beyond ten in a row do the kept colours get too old)
    live.uiBadReadbacks = live.uiChoice < 0 && hudSlot < 0 ? live.uiBadReadbacks + 1 : 0;
    const bool uiUnsafe = preUi && live.uiBadReadbacks > 10;
    live.uiReflGate = uiUnsafe ? std::max(0.0f, live.uiReflGate - 0.15f) : std::min(1.0f, live.uiReflGate + 0.1f);
    Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    // views 4 to 8: smoothness, weight, reflected colour, hit distance (in the smoothness channel), confidence
    const uint32_t reflectView = live.debugView == 4 ? 1u : live.debugView == 5 ? 2u : live.debugView == 6 ? 3u : live.debugView == 7 ? 1u :
                                 live.debugView == 8 ? 3u : live.debugView == 9 ? 4u : live.debugView == 10 ? 5u : live.debugView == 11 ? 6u : live.debugReflect;
    const bool reflections = live.reflectBuf && (live.reflEffective > 0 || reflectView);
    if (reflections)
        Transition(list.Get(), live.reflectBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    // reflections stabilised over time (real frame: history update; generated frame: the
    // last result is reused as it is)
    const bool isGenerated = live.syncNow ? !live.presentComputed : live.lastPresentDraws == 0;
    int reflHistOut = -1;
    if (reflections && live.resolvePso)
    {
        const uint64_t bytes = uint64_t(live.width) * live.height * 16;
        if (!live.reflHist[0] || live.reflHistBytes < bytes)
        {
            Retire(live.reflHist[0]), Retire(live.reflHist[1]);
            live.reflHist[0] = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.reflHist[1] = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.reflHistBytes = bytes, live.reflHistLast = -1;
        }
        if (!live.reflMotion || live.reflMotionBytes < bytes)
        {
            Retire(live.reflMotion);
            live.reflMotion = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.reflMotionBytes = live.reflMotion ? bytes : 0, live.reflMotionFrame = ~0ull;
        }
        if (live.reflHist[0] && live.reflHist[1])
        {
            if (isGenerated && live.reflHistLast >= 0)
                reflHistOut = live.reflHistLast;
            else if (!isGenerated)
            {
                const int prev = live.reflHistLast, cur = prev == 0 ? 1 : 0;
                const bool motionOk = live.mvBuf && live.mvFrame != ~0ull && live.frame <= live.mvFrame + 2 && live.mvWidth >= live.width &&
                                      live.mvHeight >= live.height && live.depthPrevBuf && prev >= 0;
                Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
                if (live.depthPrevBuf)
                    Transition(list.Get(), live.depthPrevBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
                if (live.mvBuf)
                    Transition(list.Get(), live.mvBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
                Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                if (prev >= 0)
                    Transition(list.Get(), live.reflHist[prev].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                list->SetComputeRootSignature(live.resolveRs.Get());
                list->SetPipelineState(live.resolvePso.Get());
                list->SetComputeRootShaderResourceView(0, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(1, live.reflectBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(2, live.reflHist[prev >= 0 ? prev : cur]->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(3, live.mvBuf ? live.mvBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(4, live.depthBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(5, live.depthPrevBuf ? live.depthPrevBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(6, live.sunBuf->GetGPUVirtualAddress());
                // anchoring (reflected point found in the previous frame, identities): this frame's and the previous one
                const bool anchored = live.anchorOut >= 0 && live.anchor[0] && live.anchor[1] && live.anchorFrame + 1 == live.frame; // written by this frame's effects
                if (anchored)
                    for (auto& a : live.anchor)
                        Transition(list.Get(), a.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                list->SetComputeRootShaderResourceView(7, anchored ? live.anchor[live.anchorOut]->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(8, anchored ? live.anchor[1 - live.anchorOut]->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
                const bool mixed = live.reflMixValid && live.reflMix && live.radStamp + 1 == live.frame;
                if (mixed)
                    Transition(list.Get(), live.reflMix.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                list->SetComputeRootShaderResourceView(9, mixed ? live.reflMix->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(10, live.reflHist[cur]->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(11, live.reflMotion ? live.reflMotion->GetGPUVirtualAddress() : live.reflHist[cur]->GetGPUVirtualAddress());
                const bool d24r = live.depthFormat == DXGI_FORMAT_R24G8_TYPELESS || live.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
                const struct
                {
                    uint32_t width, height, pitch, format, renderWidth, renderHeight, motionPitch, motionConvention, depthPitch, depth24;
                    float alpha, maxLuminance;
                    uint32_t flags;
                    float p00, p11;
                    float alphaMax, gamma, depthTol, anchorAlpha, anchorGamma, softPx, spatialMix;
                } rc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.mvPitch,
                         motionOk ? live.mvConvention : 255u, live.depthPitch, d24r ? 1u : 0u,
                         // (faster settling only where the water normal is exact, game camera known; RDR2's normals come from depth and are noisier)
                         live.camLive && live.worldUpKnown ? live.reflAlphaMin : std::min(live.reflAlphaMin, 0.1f), 4.0f,
                         (motionOk ? 1u : 0u) | (live.debugView == 8 ? 4u : 0u) | (anchored && live.anchorPrevOk ? 8u : 0u) | (mixed ? 16u : 0u), live.p00, live.p11,
                         live.reflAlphaMax, live.reflGamma, live.reflDepthTol, live.anchorAlpha, live.anchorGamma, live.reflSoft, live.reflSpatial };
                list->SetComputeRoot32BitConstants(12, 22, &rc, 0);
                list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
                if (mixed)
                    Transition(list.Get(), live.reflMix.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (live.reflMotion)
                {
                    D3D12_RESOURCE_BARRIER mb = {};
                    mb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    mb.UAV.pResource = live.reflMotion.Get();
                    list->ResourceBarrier(1, &mb);
                    live.reflMotionFrame = motionOk ? live.frame : ~0ull;
                }
                // surface cache: one render pixel per 4×4 block (live_surfcache), on this frame's RT scene
                if (live.surfCacheOn && live.surfPso && live.tlas && live.sceneTable && live.sceneCount && live.sceneStamp + 1 == live.frame &&
                    live.capacity)
                {
                    const uint64_t triangles = live.capacity / 3;
                    if (!live.surfCache || live.surfTriangles < triangles)
                    {
                        Retire(live.surfCache);
                        live.surfCache = UavBuffer(g.device, triangles * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // zeroed: empty
                        live.surfTriangles = live.surfCache ? triangles : 0, live.surfFilled = 0;
                        Log("URTI live: surface cache for %llu triangles (%.1f MB)", (unsigned long long)triangles, triangles * 8 / 1048576.0);
                    }
                    if (live.surfCache)
                    {
                        list->SetComputeRootSignature(live.surfRs.Get());
                        list->SetPipelineState(live.surfPso.Get());
                        list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
                        list->SetComputeRootShaderResourceView(1, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
                        list->SetComputeRootShaderResourceView(2, live.depthBuf->GetGPUVirtualAddress());
                        list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
                        list->SetComputeRootShaderResourceView(4, live.sceneTable->GetGPUVirtualAddress());
                        list->SetComputeRootUnorderedAccessView(5, live.surfCache->GetGPUVirtualAddress());
                        const bool geoColorsWrite = live.geoColors && live.geoOn;
                        list->SetComputeRootUnorderedAccessView(6, geoColorsWrite ? live.geoColors->GetGPUVirtualAddress() : live.surfCache->GetGPUVirtualAddress());
                        const struct
                        {
                            uint32_t width, height, pitch, format, renderWidth, renderHeight, depthPitch, depth24;
                            float p00, p11;
                            uint32_t regionCount, frameIndex, triangles, geoColorsOn;
                            float uiZone[4];
                        } sc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.depthPitch, d24r ? 1u : 0u,
                                 live.p00, live.p11, live.sceneCount, uint32_t(live.frame), uint32_t(live.surfTriangles), geoColorsWrite ? 1u : 0u,
                                 { live.uiZone[0], live.uiZone[1], live.uiZone[2], live.uiZone[3] } };
                        list->SetComputeRoot32BitConstants(7, 18, &sc, 0);
                        list->Dispatch((live.width / 4 + 8) / 8, (live.height / 4 + 8) / 8, 1);
                        D3D12_RESOURCE_BARRIER cb = {};
                        cb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        cb.UAV.pResource = live.surfCache.Get();
                        list->ResourceBarrier(1, &cb);
                        if (geoColorsWrite)
                        {
                            cb.UAV.pResource = live.geoColors.Get();
                            list->ResourceBarrier(1, &cb);
                        }
                        live.surfFilled++;
                    }
                }
                // world radiance cache: what the image shows, deposited where it is in the world (live_radcache); then the sky map grows
                if (mixed && live.radPso && live.radParamsNow && live.radHash && live.radEnv && live.radStats && (live.frame & 1) == 0)
                {
                    const bool sceneOk = live.tlas && live.sceneStamp + 1 == live.frame;
                    const bool stats = live.reflStatsOn;
                    if (stats)
                    {
                        if (const uint32_t* v = nullptr; live.radStatsRb[live.frame % 3] && SUCCEEDED(live.radStatsRb[live.frame % 3]->Map(0, nullptr, (void**)&v)) && v)
                        {
                            for (int k = 0; k < 7; k++)
                                live.statRad[k] += v[k];
                            live.radStatsRb[live.frame % 3]->Unmap(0, nullptr);
                            if (++live.radStatFrames == 300)
                            {
                                Log("URTI live: world radiance cache, per frame: %.0f colours deposited, %.0f refused (table full), %.0f holes and %.0f solids learned, %.0f sky pixels; %.0f cells created, %.0f reclaimed",
                                    live.statRad[0] / 300.0, live.statRad[1] / 300.0, live.statRad[2] / 300.0, live.statRad[3] / 300.0, live.statRad[4] / 300.0, live.statRad[5] / 300.0, live.statRad[6] / 300.0);
                                for (auto& v2 : live.statRad)
                                    v2 = 0;
                                live.radStatFrames = 0;
                            }
                        }
                        Transition(list.Get(), live.radStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                        list->CopyBufferRegion(live.radStats.Get(), 0, g.zeros.Get(), 0, 32);
                        Transition(list.Get(), live.radStats.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    }
                    list->SetComputeRootSignature(live.radRs.Get());
                    list->SetPipelineState(live.radPso.Get());
                    list->SetComputeRootShaderResourceView(0, sceneOk ? live.tlas->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
                    list->SetComputeRootShaderResourceView(1, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
                    list->SetComputeRootShaderResourceView(2, live.depthBuf->GetGPUVirtualAddress());
                    list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
                    list->SetComputeRootShaderResourceView(4, live.reflectBuf->GetGPUVirtualAddress());
                    list->SetComputeRootShaderResourceView(5, live.radParamsNow->GetGPUVirtualAddress());
                    list->SetComputeRootUnorderedAccessView(6, live.radHash->GetGPUVirtualAddress());
                    list->SetComputeRootUnorderedAccessView(7, live.radEnv->GetGPUVirtualAddress());
                    list->SetComputeRootUnorderedAccessView(8, live.radStats->GetGPUVirtualAddress());
                    struct
                    {
                        uint32_t width, height, pitch, format, renderWidth, renderHeight, depthPitch, depth24;
                        float p00, p11;
                        uint32_t mode, flags;
                        float uiZone[4];
                        float maxLuminance;
                    } xc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.depthPitch, d24r ? 1u : 0u,
                             live.p00, live.p11, 0u, (sceneOk ? 1u : 0u) | (stats ? 2u : 0u),
                             { live.uiZone[0], live.uiZone[1], live.uiZone[2], live.uiZone[3] }, 8.0f };
                    list->SetComputeRoot32BitConstants(9, 17, &xc, 0);
                    list->Dispatch(((live.width + 1) / 2 + 7) / 8, ((live.height + 1) / 2 + 7) / 8, 1);
                    D3D12_RESOURCE_BARRIER xb[2] = {};
                    xb[0].Type = xb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    xb[0].UAV.pResource = live.radHash.Get(), xb[1].UAV.pResource = live.radEnv.Get();
                    list->ResourceBarrier(2, xb);
                    xc.mode = 1;
                    list->SetComputeRoot32BitConstants(9, 17, &xc, 0);
                    list->Dispatch((live.radEnvSize + 7) / 8, (live.radEnvSize + 7) / 8, 1);
                    list->ResourceBarrier(1, &xb[1]);
                    if (stats && live.radStatsRb[live.frame % 3])
                    {
                        Transition(list.Get(), live.radStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        list->CopyBufferRegion(live.radStatsRb[live.frame % 3].Get(), 0, live.radStats.Get(), 0, 28);
                        Transition(list.Get(), live.radStats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    }
                }
                if (anchored)
                    for (auto& a : live.anchor)
                        Transition(list.Get(), a.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (prev >= 0)
                    Transition(list.Get(), live.reflHist[prev].Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (live.mvBuf)
                    Transition(list.Get(), live.mvBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
                if (live.depthPrevBuf)
                    Transition(list.Get(), live.depthPrevBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
                Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_RESOURCE_BARRIER ub = {};
                ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                ub.UAV.pResource = live.reflHist[cur].Get();
                list->ResourceBarrier(1, &ub);
                live.reflHistLast = cur;
                reflHistOut = cur;
            }
        }
        if (reflHistOut >= 0)
            Transition(list.Get(), live.reflHist[reflHistOut].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        // smoothed colour of the water (once per computed frame)
        if (reflHistOut >= 0 && !isGenerated && live.waterBaseOn > 0 && live.waterBasePso && live.camLive && live.worldUpKnown && live.reflectUp)
        {
            const uint32_t lw = (live.width + 7) / 8, lh = (live.height + 7) / 8;
            const uint64_t lowBytes = uint64_t(lw) * lh * 16;
            if (!live.waterBase[0] || live.waterBaseW != lw || live.waterBaseH != lh)
            {
                for (auto& b : live.waterBase)
                    Retire(b), b = UavBuffer(g.device, lowBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                live.waterBaseW = lw, live.waterBaseH = lh;
            }
            if (live.waterBase[0] && live.waterBase[1])
            {
                list->SetComputeRootSignature(live.waterBaseRs.Get());
                list->SetPipelineState(live.waterBasePso.Get());
                list->SetComputeRootShaderResourceView(0, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(1, live.reflHist[reflHistOut]->GetGPUVirtualAddress());
                uint32_t wc[12] = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, lw, lh, 0, 0, 0, 0 };
                list->SetComputeRootShaderResourceView(2, live.reflHist[reflHistOut]->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(3, live.waterBase[0]->GetGPUVirtualAddress());
                list->SetComputeRoot32BitConstants(4, 12, wc, 0);
                list->Dispatch((lw + 7) / 8, (lh + 7) / 8, 1);
                Transition(list.Get(), live.waterBase[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                wc[8] = 1;
                list->SetComputeRootShaderResourceView(2, live.waterBase[0]->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(3, live.waterBase[1]->GetGPUVirtualAddress());
                list->SetComputeRoot32BitConstants(4, 12, wc, 0);
                list->Dispatch((lw + 7) / 8, (lh + 7) / 8, 1);
                Transition(list.Get(), live.waterBase[0].Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                D3D12_RESOURCE_BARRIER wb = {};
                wb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                wb.UAV.pResource = live.waterBase[1].Get();
                list->ResourceBarrier(1, &wb);
                live.waterBaseFrame = live.frame;
            }
        }
    }
    LiveTimeD(list.Get(), 14);
    // sun share in the game's image (live_sunlight): points of the same surface that the game lights and shadows
    if (live.sunMeasure && !isGenerated && live.sunlightPso && live.atlasBuf && live.atlasFrame + 1 == live.frame && live.depthBuf && // copied by this frame's effects
        live.calibrated && live.framesSinceSun < 30)
    {
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.sunBuf.Get(), 1232, g.zeros.Get(), 0, 2048 - 1232); // (not the camera orientation, 2048 and after)
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
        Transition(list.Get(), live.atlasBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
        list->SetComputeRootSignature(live.sunlightRs.Get());
        list->SetPipelineState(live.sunlightPso.Get());
        list->SetComputeRootShaderResourceView(0, live.depthBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, live.atlasBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3, live.sunBuf->GetGPUVirtualAddress());
        const bool d24s = live.depthFormat == DXGI_FORMAT_R24G8_TYPELESS || live.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
        const struct
        {
            uint32_t width, height, depthPitch, depth24;
            float A, B, p00, p11, sunSign, gameBias;
            uint32_t atlasPitch, atlasFormat, imageWidth, imageHeight, imagePitch, format, frameIndex, pad0, pad1, pad2;
        } lc = { live.width, live.height, live.depthPitch, d24s ? 1u : 0u, live.A, live.B, live.p00, live.p11, live.sunSign, g_liveGameBias,
                 live.atlasPitch, live.atlas16 ? 2u : live.atlasD24 ? 1u : 0u, uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, uint32_t(live.frame), 0, 0, 0 };
        list->SetComputeRoot32BitConstants(4, 20, &lc, 0);
        list->Dispatch((live.width / 4 + 7) / 8, (live.height / 4 + 7) / 8, 1);
        Transition(list.Get(), live.atlasBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = live.sunBuf.Get();
        list->ResourceBarrier(1, &ub);
    }
    // layer queue: this computed frame's layer stored with its number; the composition takes the one of the frame the screen shows
    bool ringUse = false;
    if (hudLead && live.delayPso && live.hudGain && live.shadowBuf && live.syncNow)
    {
        const uint64_t sliceBytes = uint64_t(live.width) * live.height * 20;
        if (!live.delayRing || live.delayW != live.width || live.delayH != live.height)
        {
            Retire(live.delayRing);
            live.delayRing = UavBuffer(g.device, 64 + 3 * sliceBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // zeroed: no slice filled
            live.delayW = live.width, live.delayH = live.height;
        }
        if (live.delayRing && live.presentComputed && !live.layerSuspect)
        {
            const bool motionHere = live.reflMotion && live.reflMotionFrame == live.frame;
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            if (motionHere)
                Transition(list.Get(), live.reflMotion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.delayRs.Get());
            list->SetPipelineState(live.delayPso.Get());
            list->SetComputeRootShaderResourceView(0, live.shadowBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, reflHistOut >= 0 ? live.reflHist[reflHistOut]->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, motionHere ? live.reflMotion->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, live.delayRing->GetGPUVirtualAddress());
            const uint32_t dc[8] = { live.width, live.height, live.shadowPitch, uint32_t(live.layerHudSerial % 3), uint32_t(live.layerHudSerial),
                                     (reflHistOut >= 0 ? 1u : 0u) | (motionHere ? 2u : 0u), 0, 0 };
            list->SetComputeRoot32BitConstants(5, 8, dc, 0);
            list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER rb = {};
            rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            rb.UAV.pResource = live.delayRing.Get();
            list->ResourceBarrier(1, &rb);
            if (motionHere)
                Transition(list.Get(), live.reflMotion.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        ringUse = live.delayRing != nullptr;
    }
    LiveTimeD(list.Get(), 15);
    list->SetComputeRootSignature(live.compositeRs.Get());
    list->SetPipelineState(live.compositePso.Get());
    list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, reflections ? live.reflectBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    // generated frame: built between the previous real frame and the one whose computation is the last done (the
    // computation happens as soon as the game's draws are executed: with DLSS-G, just before the generated frame); blend
    // mode: half of each
    const bool generated = live.imageGenerated;
    const bool blend = generated && live.fgMode == 2 && live.prevShadowBuf;
    if (blend)
        Transition(list.Get(), live.prevShadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(3, blend ? live.prevShadowBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(4, reflHistOut >= 0 ? live.reflHist[reflHistOut]->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(5, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootShaderResourceView(6, live.sunBuf->GetGPUVirtualAddress());
    if (live.uiCounts)
        Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootShaderResourceView(7, live.uiCounts ? live.uiCounts->GetGPUVirtualAddress() : live.sunBuf->GetGPUVirtualAddress());
    // this frame's light bounce and the surfaces' own colour (copied during the game frame)
    // light bounce: this computation's, or the last one computed when nothing new was computed (generated frame)
    // (counted generously, like the mask: with frame generation, several presents in a row without a computation or without
    // a copied G-buffer arrive in normal operation; the bounce cut on one of them blinked)
    const bool giFresh = live.giStamp != ~0ull && live.frame <= live.giStamp + 8;
    const float giEffC = live.giStrength * (live.menuGi ? live.menuGiForce : 0.0f);
    const bool giUse = giEffC > 0 && live.giValid && giFresh && live.albedoBuf && live.albedoFrame != ~0ull &&
                       live.frame <= live.albedoFrame + 8 && live.albedoWidth >= live.width && live.albedoHeight >= live.height;
    const bool normalUse = giUse && live.personMask && live.normalBuf && live.normalFrame != ~0ull && live.frame <= live.normalFrame + 8 && live.normalWidth >= live.width &&
                           live.normalHeight >= live.height;
    if (giUse)
    {
        Transition(list.Get(), live.giBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        Transition(list.Get(), live.albedoBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    }
    if (normalUse)
        Transition(list.Get(), live.normalBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(8, giUse ? live.giBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(9, giUse ? live.albedoBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    // view 11 (alignment): the computation's depth instead of the normal
    const bool alignView = live.debugView == 11 && live.depthBuf;
    if (alignView)
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(10, alignView ? live.depthBuf->GetGPUVirtualAddress() : normalUse ? live.normalBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    if (const uint64_t maskBytes = uint64_t(bd.Width) * bd.Height * 4; !live.uiMaskBuf || live.uiMaskBytes < maskBytes)
        Retire(live.uiMaskBuf), live.uiMaskBuf = UavBuffer(g.device, maskBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), live.uiMaskBytes = maskBytes;
    // generated frame: the last computation's shadow and reflection slide with the motion (measured by the last computation, one or two presents before)
    const bool shiftNow = live.fgShift > 0 && live.syncNow && !live.presentComputed && live.reflMotion && live.reflMotionFrame != ~0ull &&
                          live.frame <= live.reflMotionFrame + 2;
    if (live.reflMotion)
        Transition(list.Get(), live.reflMotion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootShaderResourceView(11, live.reflMotion ? live.reflMotion->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    const bool waterBaseUse = live.waterBaseOn > 0 && live.waterBase[1] && live.waterBaseFrame != ~0ull && live.frame <= live.waterBaseFrame + 8 && live.camLive;
    if (waterBaseUse)
        Transition(list.Get(), live.waterBase[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootShaderResourceView(12, waterBaseUse ? live.waterBase[1]->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    const bool leadUse = ringUse || (hudLead && live.hudGain && live.reflMotion && live.reflMotionFrame != ~0ull && live.frame <= live.reflMotionFrame + 3);
    if (leadUse)
        Transition(list.Get(), live.hudGain.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootShaderResourceView(13, leadUse ? live.hudGain->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    if (ringUse)
        Transition(list.Get(), live.delayRing.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootShaderResourceView(14, ringUse ? live.delayRing->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(15, live.imageOut->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(16, live.uiMaskBuf ? live.uiMaskBuf->GetGPUVirtualAddress() : live.imageOut->GetGPUVirtualAddress());
    const bool tinted = live.sunMeasure && live.sunSkyFrames && live.sunSky[3] > 0.01f; // measured shadow tint
    const struct
    {
        uint32_t width, height, pitch, format, maskWidth, maskHeight, maskPitch;
        float strength;
        uint32_t split, showMask;
        float reflStrength;
        uint32_t debugReflect;
        float p00, p11;
        float tint[3];
        uint32_t uiParity;
        float uiZone[4];
        float giStrength;
        uint32_t albedoPitch, normalPitch;
        float fgShift;
        uint32_t waterBaseSize;
        float waterBaseOn, waterGain;
    } cc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.shadowPitch, strength,
             split ? 1u : 0u, showMask ? 1u : 0u,
             // (generated frame too: the last stabilised reflection. Cut on them, it went out every other present)
             reflections ? (reflectView ? 1.0f : live.reflEffective * live.uiReflGate) : 0.0f,
             (reflections ? reflectView : 0u) | (blend ? 0x100u : 0u) | (reflHistOut >= 0 ? 0x200u : 0u) | (preUi ? 0x400u : 0u) |
             (live.sunShadows ? 0u : 0x800u) | (leadUse && !ringUse ? 0x1000u : 0u) | (ringUse ? 0x2000u : 0u), live.p00, live.p11,
             { tinted ? live.sunSky[0] / live.sunSky[3] : 1.0f, tinted ? live.sunSky[1] / live.sunSky[3] : 1.0f, tinted ? live.sunSky[2] / live.sunSky[3] : 1.0f },
             uint32_t((live.uiFrame - 1) & 1), { live.uiZone[0], live.uiZone[1], live.uiZone[2], live.uiZone[3] },
             giUse ? giEffC : 0.0f, live.albedoPitch, alignView ? live.depthPitch : normalUse ? live.normalPitch : 0u,
             shiftNow ? live.fgShift : 0.0f, waterBaseUse ? (live.waterBaseW | (live.waterBaseH << 16)) : 0u, waterBaseUse ? live.waterBaseOn : 0.0f, live.waterGain };
    list->SetComputeRoot32BitConstants(17, 29, &cc, 0);
    list->Dispatch((cc.width + 7) / 8, (cc.height + 7) / 8, 1);
    if (ringUse)
        Transition(list.Get(), live.delayRing.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (leadUse)
        Transition(list.Get(), live.hudGain.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (waterBaseUse)
        Transition(list.Get(), live.waterBase[1].Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (live.reflMotion)
        Transition(list.Get(), live.reflMotion.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.statShifted += shiftNow ? 1u : 0u;
    LiveTimeD(list.Get(), 16);
    live.diagReflStrength = cc.reflStrength, live.diagDebugReflect = cc.debugReflect, live.diagHistOut = reflHistOut;
    live.diagPreUi = preUi != nullptr, live.diagSnaps = snapCount;
    if (giUse)
    {
        Transition(list.Get(), live.giBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list.Get(), live.albedoBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (normalUse)
        Transition(list.Get(), live.normalBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    if (alignView)
        Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (live.uiCounts)
        Transition(list.Get(), live.uiCounts.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list.Get(), live.shadowBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (blend)
        Transition(list.Get(), live.prevShadowBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    if (reflHistOut >= 0)
        Transition(list.Get(), live.reflHist[reflHistOut].Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (reflections)
        Transition(list.Get(), live.reflectBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list.Get(), live.imageIn.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    for (uint32_t k = 0; k < snapCount; k++)
        Transition(list.Get(), live.snaps[k].Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    if (preUi)
        Transition(list.Get(), preUi, srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    buf.pResource = live.imageOut.Get();
    list->CopyTextureRegion(&tex, 0, 0, 0, &buf, nullptr);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Transition(list.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (live.timeHeap && live.timeReadback[slotTime])
    {
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 7);
        LiveTimeD(list.Get(), 17);
        if (live.timeDHeap && live.timeCRb[live.compCounter % 3])
            list->ResolveQueryData(live.timeDHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, uint32_t(live.compCounter % 3) * LiveState::kTimeD + 12, 6,
                                   live.timeCRb[live.compCounter % 3].Get(), 0);
        list->ResolveQueryData(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 6, 2, live.timeReadback[slotTime].Get(), 6 * 8);
    }
    live.statComposited++;
    live.composedNow = true;
}

bool OnOpenOverlay(effect_runtime*, bool open, input_source)
{
    live.overlayOpen = open;
    Log("URTI live: ReShade menu %s", open ? "open" : "closed");
    return false;
}

// screen snapshot after the effects (trigger file urti_photo, or presents listed in urti.ini
// photo_frames=: urti_photo_<present>.ppm, to compare two launches frame by frame)
void OnReshadePresent(effect_runtime* runtime)
{
    if (!OurDevice(runtime->get_device()))
        return;
    // "URTI" menu checkbox: unticked, no RT computation at all (capture, structure, shadows, reflections, composition);
    // F7 does the same and the box follows
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiOn"); v.handle && g_liveAtStart)
    {
        bool on = true;
        runtime->get_uniform_value_bool(v, &on, 1);
        if (int(on) != live.menuOn)
        {
            if (live.menuOn >= 0 && live.enabled != on)
                Log("URTI: %s (menu)", on ? "on, ray-tracing computation resumes" : "off, no ray-tracing computation at all");
            live.menuOn = int(on), live.enabled = on;
        }
        else if (live.enabled != on)
            runtime->set_uniform_value_bool(v, live.enabled), live.menuOn = int(live.enabled); // F7
    }
    // menu checkboxes: reflections, ambient occlusion (strength, range), light bounce (strength)
    {
        auto readBool = [&](const char* name, bool& out) {
            if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", name); v.handle)
            {
                bool b = out;
                runtime->get_uniform_value_bool(v, &b, 1);
                if (b != out)
                    Log("URTI: menu, %s %s", name, b ? "ticked" : "unticked");
                out = b;
            }
        };
        auto readFloat = [&](const char* name, float& out, float lo, float hi) {
            if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", name); v.handle)
            {
                float f = out;
                runtime->get_uniform_value_float(v, &f, 1);
                if (std::isfinite(f))
                    out = std::clamp(f, lo, hi);
            }
        };
        readBool("UrtiReflections", live.menuRefl), readBool("UrtiOcclusion", live.menuAo), readBool("UrtiBounce", live.menuGi), readBool("UrtiGloss", live.menuGloss);
        readFloat("UrtiOcclusionStrength", live.menuAoForce, 0.0f, 2.0f), readFloat("UrtiOcclusionRange", live.menuAoRadius, 0.5f, 5.0f);
        readFloat("UrtiBounceStrength", live.menuGiForce, 0.0f, 3.0f), readFloat("UrtiSky", live.menuSky, 0.0f, 1.6f);
        // occlusion and bounce unticked: the sun is no longer used, its map is only replayed by eighths (2 million vertices per present in Red Dead)
        g_liveSunEighth = !live.sunShadows && (live.sunEighth || !(live.menuAo || live.menuGi));
        // (game without a read camera, field of view measured automatically: sun tracking stays, its cascades measure the field of view, which
        // reflections need for correct angles)
        g_liveSunUnused = !live.sunShadows && !live.menuAo && !live.menuGi && (live.camLive || !live.fovAuto);
    }
    LiveComposite(runtime);
    // film: this present (computed or generated), before and after URTI, stored in the video-memory ring
    if (live.film > 0 && live.composedNow && live.imageIn && live.imageOut && live.filmPso)
        if (const resource back = runtime->get_current_back_buffer(); back.handle)
        {
            ID3D12Resource* bb = reinterpret_cast<ID3D12Resource*>(back.handle);
            const D3D12_RESOURCE_DESC bd = bb->GetDesc();
            const uint32_t fmt = bd.Format == DXGI_FORMAT_R10G10B10A2_UNORM || bd.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? 2u :
                                 bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || bd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? 1u :
                                 bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || bd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? 0u : 9u;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            const uint32_t hw = uint32_t(bd.Width) / 2, hh = bd.Height / 2;
            const uint64_t frameBytes = uint64_t(hw) * hh * 4;
            if (fmt == 9)
                Log("URTI live: film impossible (screen image format %d)", int(bd.Format)), live.film = 0;
            else
            {
                if (!live.filmRing || live.filmFrameBytes != frameBytes)
                {
                    Retire(live.filmRing);
                    live.filmRing = UavBuffer(g.device, frameBytes * 2 * LiveState::kFilmFrames, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    live.filmFrameBytes = frameBytes;
                }
                live.filmWidth = hw, live.filmHeight = hh, live.filmFormat = fmt;
                ComPtr<ID3D12GraphicsCommandList> fl;
                if (live.filmRing && SUCCEEDED(reinterpret_cast<IUnknown*>(runtime->get_command_queue()->get_immediate_command_list()->get_native())->QueryInterface(IID_PPV_ARGS(&fl))))
                {
                    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    Transition(fl.Get(), live.imageIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
                    Transition(fl.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                    fl->SetComputeRootSignature(live.filmRs.Get());
                    fl->SetPipelineState(live.filmPso.Get());
                    for (uint32_t which = 0; which < 2; which++)
                    {
                        const uint32_t fc[8] = { hw, hh, fp.Footprint.RowPitch, uint32_t((uint64_t(live.filmIndex) * 2 + which) * frameBytes), fmt, 0, 0, 0 };
                        fl->SetComputeRootShaderResourceView(0, (which ? live.imageOut : live.imageIn)->GetGPUVirtualAddress());
                        fl->SetComputeRootUnorderedAccessView(1, live.filmRing->GetGPUVirtualAddress());
                        fl->SetComputeRoot32BitConstants(2, 8, fc, 0);
                        fl->Dispatch((hw + 7) / 8, (hh + 7) / 8, 1);
                    }
                    Transition(fl.Get(), live.imageOut.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    Transition(fl.Get(), live.imageIn.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
                    Log("FILM %u %u frame %llu generated %d computed %d sync %d draws %u", live.filmCount, live.filmIndex, (unsigned long long)live.frame,
                        int(live.imageGenerated), int(live.presentComputed), int(live.syncNow), live.lastPresentDraws);
                    live.filmIndex++;
                }
                if (live.filmIndex >= LiveState::kFilmFrames && live.filmRing)
                {
                    // end: the whole ring read at once, then written (a single wait, after the film)
                    runtime->get_command_queue()->flush_immediate_command_list();
                    ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
                    const uint64_t all = frameBytes * 2 * LiveState::kFilmFrames;
                    ComPtr<ID3D12Resource> rb = MakeBuffer(g.device, all, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                    if (rb)
                    {
                        BeginList();
                        Transition(g.list.Get(), live.filmRing.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        g.list->CopyBufferRegion(rb.Get(), 0, live.filmRing.Get(), 0, all);
                        Transition(g.list.Get(), live.filmRing.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        Submit(queue);
                        const uint8_t* data = nullptr;
                        if (SUCCEEDED(rb->Map(0, nullptr, (void**)&data)) && data)
                        {
                            const uint32_t dxgi = fmt == 2 ? uint32_t(DXGI_FORMAT_R10G10B10A2_UNORM) : fmt == 1 ? uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM) : uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM);
                            for (uint32_t k = 0; k < LiveState::kFilmFrames; k++)
                                for (uint32_t which = 0; which < 2; which++)
                                    if (FILE* f = std::fopen((live.filmFolder + (which ? "\\after_" : "\\before_") + std::to_string(k) + ".raw").c_str(), "wb"))
                                    {
                                        std::fprintf(f, "URTIRAW %u %u %u 4\n", dxgi, hw, hh);
                                        std::fwrite(data + (uint64_t(k) * 2 + which) * frameBytes, 1, size_t(frameBytes), f);
                                        std::fclose(f);
                                    }
                            rb->Unmap(0, nullptr);
                        }
                    }
                    // depth and material (water mask) of the last computation, and what is needed to rebuild each pixel's height
                    {
                        const bool d24 = live.depthFormat == DXGI_FORMAT_R24G8_TYPELESS || live.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
                        ID3D12Resource* mat = live.materialIndexNow >= 0 ? live.materialSnap[live.materialIndexNow].Get() : nullptr;
                        const uint64_t depthBytes = uint64_t(live.depthPitch) * live.allocHeight, matBytes = mat ? uint64_t(live.materialPitch) * live.materialHeight : 0;
                        ComPtr<ID3D12Resource> rd = live.depthBuf ? MakeBuffer(g.device, depthBytes + matBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST) : nullptr;
                        if (rd)
                        {
                            BeginList();
                            Transition(g.list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                            g.list->CopyBufferRegion(rd.Get(), 0, live.depthBuf.Get(), 0, depthBytes);
                            Transition(g.list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                            if (mat)
                            {
                                Transition(g.list.Get(), mat, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                                g.list->CopyBufferRegion(rd.Get(), depthBytes, mat, 0, matBytes);
                                Transition(g.list.Get(), mat, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                            }
                            Submit(queue);
                            const uint8_t* data = nullptr;
                            if (SUCCEEDED(rd->Map(0, nullptr, (void**)&data)) && data)
                            {
                                if (FILE* f = std::fopen((live.filmFolder + "\\depth.bin").c_str(), "wb"))
                                    std::fwrite(data, 1, size_t(depthBytes), f), std::fclose(f);
                                if (mat)
                                    if (FILE* f = std::fopen((live.filmFolder + "\\material.bin").c_str(), "wb"))
                                        std::fwrite(data + depthBytes, 1, size_t(matBytes), f), std::fclose(f);
                                rd->Unmap(0, nullptr);
                            }
                        }
                        Log("FILMCAM A %.9g B %.9g p00 %.6f p11 %.6f up %.5f %.5f %.5f d24 %d pitch %u render %ux%u alloc %ux%u material %u %ux%u format %u channel %u inverted %d",
                            live.A, live.B, live.p00, live.p11, live.worldUp[0], live.worldUp[1], live.worldUp[2], int(d24), live.depthPitch, live.width, live.height,
                            live.allocWidth, live.allocHeight, mat ? live.materialPitch : 0u, live.materialWidth, live.materialHeight, live.materialFormat, live.reflChannel,
                            int(live.reflInvert));
                    }
                    Log("URTI live: film %u finished (%u presents)", live.filmCount, live.filmIndex);
                    live.film = 0;
                    Retire(live.filmRing), live.filmFrameBytes = 0;
                }
            }
        }
    // burst: the image without URTI (its copy, imageIn) and with it, on every composed real frame
    if (live.burst > 0 && live.composedNow && (live.syncNow ? live.presentComputed : live.lastPresentDraws > 0) && live.imageIn)
        if (const resource back = runtime->get_current_back_buffer(); back.handle)
        {
            runtime->get_command_queue()->flush_immediate_command_list();
            ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
            const std::string base = live.burstFolder + "\\";
            SaveBackBuffer(queue, reinterpret_cast<ID3D12Resource*>(back.handle), base + "after_" + std::to_string(live.burstIndex) + ".ppm", 2);
            SaveBackBuffer(queue, reinterpret_cast<ID3D12Resource*>(back.handle), base + "before_" + std::to_string(live.burstIndex) + ".ppm", 2, live.imageIn.Get());
            // snapshot before the UI: the first one taken in this frame (raw) and the one URTI kept
            if (live.snaps[0])
                SaveBackBuffer(queue, reinterpret_cast<ID3D12Resource*>(back.handle), base + "photo_" + std::to_string(live.burstIndex) + ".ppm", 2, live.snaps[0].Get());
            if (live.preUiBuf)
                SaveBackBuffer(queue, reinterpret_cast<ID3D12Resource*>(back.handle), base + "gardee_" + std::to_string(live.burstIndex) + ".ppm", 2, live.preUiBuf.Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Log("BURST %u %u frame %llu RT objects %zu in %u out %u draws %u scene %u reflection %.2f hist %d bits %x gate %.2f photos %u/%d bad %u material %lld", live.burstCount, live.burstIndex, (unsigned long long)live.frame, live.admitted.size(), live.churnIn,
                live.churnOut, live.lastDrawsThisFrame, live.sceneCount, live.diagReflStrength, live.diagHistOut, live.diagDebugReflect, live.uiReflGate, live.diagSnaps, int(live.diagPreUi), live.uiBadReadbacks,
                (long long)live.frame - (long long)live.materialFrame);
            live.burstIndex++;
            if (--live.burst == 0)
                Log("URTI live: burst %u finished (%u image pairs)", live.burstCount, live.burstIndex);
        }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveSnapCount = 0; // snapshots of the next frame
    }
    if (++live.statPresents == 300)
    {
        Log("URTI live: over 300 presents, %u without a game draw, %u passes through the effects, %u with RT effects applied; %u computations without depth, %u with a mask too old, %u presents at most between two computations; %u with the ReShade menu open, %u effect off; %u computations with the frame's depth, %u with the current one, %u markers refused, %u copies; %u ring waits; material: %u waits, %u computations without their frame's; %u frames skipped (lag)",
            live.statNoDraw, live.statEffects, live.statComposited, live.statNoDepth, live.statMaskOld, live.statGapMax, live.statSkipOverlay, live.statSkipTechnique, live.statSnapUsed, live.statSnapFallback, g_liveMarksRefused, g_liveSnapCopies, live.statSlotWaits, live.statMaterialWait, live.statMaterialLate, live.statFramesSkipped);
        live.statNoDepth = live.statMaskOld = live.statGapMax = live.statSkipOverlay = live.statSkipTechnique = live.statSnapUsed = live.statSnapFallback = 0;
        g_liveMarksRefused = g_liveSnapCopies = 0;
        live.statSlotWaits = live.statMaterialWait = live.statMaterialLate = live.statFramesSkipped = 0;
        float hudGainNow = 0, hudUiNow = 0;
        if (const float* gv = nullptr; live.hudGainReadback[(live.uiFrame + 1) % 3] && SUCCEEDED(live.hudGainReadback[(live.uiFrame + 1) % 3]->Map(0, nullptr, (void**)&gv)) && gv)
            hudGainNow = gv[0], hudUiNow = gv[1], live.hudGainReadback[(live.uiFrame + 1) % 3]->Unmap(0, nullptr);
        if (live.statHudless || g_liveHudHandle)
            Log("URTI live: the game's UI-free image taken %u times over 300 presents (%u conversions); screen image = %.3f × UI-free image; "
                "slope %.3f; UI %.1f %% of the screen; %u presents show a copy's image (mask redone: %u the last, %u the one before, %u earlier), mean match %.2f; "
                "mean layer lead over the image %.2f game frames (%s); %u catch-up layers discarded; copy of the computation: %u the last, %u the one before, %u earlier, %u not there yet",
                live.statHudless, live.statHudConverted, hudGainNow, live.hudSlopeNow, hudUiNow * 100.0f, live.statHudMatched, live.statHudLag[0], live.statHudLag[1], live.statHudLag[2],
                live.statHudless ? live.statHudMatchSum / live.statHudless : 0.0, live.statHudless ? live.statLeadSum / live.statHudless : 0.0, live.lagOn ? "caught up" : "left", live.statSuspect,
                live.statHudConvBack[0], live.statHudConvBack[1], live.statHudConvBack[2], live.statHudConvBack[3]);
        live.statHudConvBack[0] = live.statHudConvBack[1] = live.statHudConvBack[2] = live.statHudConvBack[3] = 0, live.statSuspect = 0;
        live.statHudConverted = 0, live.statHudMatched = 0, live.statHudMatchSum = 0, live.statLeadSum = 0;
        live.statHudLag[0] = live.statHudLag[1] = live.statHudLag[2] = 0;
        live.statHudless = 0;
        Log("URTI live: generated frames: shadow and reflection slid %u times over 300 presents (share %.2f of the motion)", live.statShifted, live.fgShift);
        live.statShifted = 0;
        if (g_liveCensusOn)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            static const char* const kDepthNames[4] = { "nodepth", "main", "otherview", "other" };
            static const char* const kClass[8] = { "written", "transp", "notest", "tess", "otherlevel", "rest", "?", "unknown" };
            for (const auto& [key, ce] : g_liveCensus)
            {
                std::vector<std::pair<uint32_t, uint32_t>> big(ce.sizes.begin(), ce.sizes.end());
                std::sort(big.begin(), big.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                char top[220] = "";
                for (size_t k = 0; k < big.size() && k < 8; k++)
                {
                    char item[32];
                    std::snprintf(item, sizeof(item), " %ux%u", big[k].first, big[k].second);
                    std::strncat(top, item, sizeof(top) - std::strlen(top) - 1);
                }
                Log("CENSUS %s %s %s%s: %llu draws, %llu vertices;%s", kDepthNames[key >> 8], kClass[(key >> 4) & 15], (key & 2) ? "blended" : "opaque", (key & 1) ? " indirect" : "",
                    (unsigned long long)ce.draws, (unsigned long long)ce.vertices, top);
            }
            g_liveCensus.clear();
        }
        if (live.geoOn && live.statGeoFrames)
        {
            uint64_t triangles = 0, usedBytes = 0;
            for (const auto& [id, e] : live.geo)
                triangles += e.triangles, usedBytes += e.size;
            const double frames = double(live.statGeoFrames);
            Log("URTI live: geometry cache: %zu objects, %.2f M triangles, %.0f MB out of %.0f; built per frame: %.1f objects, %.0f vertices; %llu evicted from the reserve, %u clearings, %u motions impossible to measure; camera %.1f m from the frame; %llu game-queue waits",
                live.geo.size(), triangles / 1e6, usedBytes / 1048576.0, live.geoPoolBytes / 1048576.0, live.statGeoBuilds / frames, live.statGeoVerts / frames,
                (unsigned long long)live.statGeoEvicted, live.statGeoFlushes, live.statGeoBadFit, live.geoPoseDistance, (unsigned long long)live.statSyncWaits);
            live.statSyncWaits = 0;
            Log("URTI live: geometry cache: %llu objects close to the camera discarded (closer than 6 m), share per frame: %u new vertices, %u refreshed",
                (unsigned long long)live.statGeoNear, live.geoNewVerts, live.geoRefreshVerts);
            live.statGeoNear = 0;
            live.statGeoFrames = live.statGeoBuilds = live.statGeoVerts = live.statGeoEvicted = 0;
            live.statGeoFlushes = live.statGeoBadFit = 0;
        }
        Log("URTI live: draws executed: %u written in the same present window (%u lists), %u written before the previous "
            "present (%u lists)", g_execOnTime.exchange(0), g_execLists.exchange(0), g_execLate.exchange(0), g_execLateLists.exchange(0));
        live.statPresents = live.statNoDraw = live.statEffects = live.statComposited = 0;
    }
    if (std::binary_search(g_photoFrames.begin(), g_photoFrames.end(), g.frame))
    {
        if (const resource back = runtime->get_current_back_buffer(); back.handle)
        {
            runtime->get_command_queue()->flush_immediate_command_list();
            SaveBackBuffer(reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()), reinterpret_cast<ID3D12Resource*>(back.handle),
                           g_dir + "\\urti_photo_" + std::to_string(g.frame) + ".ppm", 1);
        }
        return;
    }
    const std::string trigger = g_dir + "\\urti_photo";
    // not before ReShade has finished loading its effects (a few tenths of a second)
    if (g.frame < 300 || g.frame % 15 != 0 || GetFileAttributesA(trigger.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    if (live.enabled && (!live.bound || live.frame < live.boundFrame + 60))
        return; // effects not ready yet: the snapshot waits
    DeleteFileA(trigger.c_str());
    // direct read of the display buffer (handles the game's HDR10, which capture_screenshot returns raw)
    const resource back = runtime->get_current_back_buffer();
    if (!back.handle)
        return;
    const std::string path = g_dir + "\\urti_photo" + std::to_string(live.photoIndex++) + ".ppm";
    runtime->get_command_queue()->flush_immediate_command_list(); // this frame's effects first
    SaveBackBuffer(reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()), reinterpret_cast<ID3D12Resource*>(back.handle), path, 1);
    Log("URTI: snapshot %s (RT effects %s)", path.c_str(), live.enabled ? "on" : "off");
}

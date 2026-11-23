// URTI (Universal Ray Tracing Injector), a ReShade add-on: captures the geometry of a DirectX 12 game
// without touching its rendering, and adds ray-traced effects (urti_live.inl).
//
// For each graphics pipeline of the game, a "shadow" pipeline: same vertex shader, same root
// signature, stream output of SV_Position, no rasterisation and no pixel shader. During
// the captured frame, each draw (direct or ExecuteIndirect) is replayed once with that pipeline:
// we get the triangles exactly as the game transformed them (animation, instancing, compression
// included), in clip space. Test: these triangles, rasterised again, must reproduce
// the game's depth (no camera needed).
//
// D3D12 requires ALLOW_STREAM_OUTPUT in the root signature: it is added to the game's at their
// creation (ID3D12Device::CreateRootSignature).
//
// Caution in a real game:
//  - a game often creates several D3D12 devices (tests, upscalers): only the one that presents
//    the image is used, chosen at the first present;
//  - the depth is copied at the next clear, where D3D12 guarantees its state (depth
//    write), rather than at the present where its state is unknown.
//
// Settings: urti.ini next to the add-on (capture_frames=3000,6000; otherwise the F9 key), or the environment
// variable URTI_CAPTURE_FRAME. Outputs next to the add-on (or in URTI_OUT): urti_log.txt,
// depths of the game and of the captured geometry (PFM), image of the frame (PPM).

#include <imgui.h>
#include <reshade.hpp>

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <mutex>
#include <fstream>
#include <shared_mutex>
#include <string>
#include <thread>
#include <emmintrin.h>
#include <smmintrin.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if URTI_NRD
// NRD and NRI (NVIDIA): headers at global scope, the integration code is in urti_nrd.inl
#include <windows.h>
#include <dbghelp.h>
#include <NRD.h>
#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIWrapperD3D12.h>
// internal failures of the integration layer made visible (silent in Release)
#define NRD_INTEGRATION_ASSERT(expr, msg) do { if (!(expr)) { std::fprintf(stderr, "NRD: %s\n", msg); std::fflush(stderr); } } while (0)
#include <NRDIntegration.hpp>
#endif

using Microsoft::WRL::ComPtr;
using namespace reshade::api;

extern "C" __declspec(dllexport) const char* NAME = "URTI";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Universal Ray Tracing Injector: ray-traced reflections for DirectX 12 games.";

namespace
{
// ------------------------------------------------------------------------------------------------
// log and settings
// ------------------------------------------------------------------------------------------------

std::string g_dir, g_logPath;
std::vector<uint64_t> g_captureFrames;
std::vector<uint64_t> g_photoFrames; // urti.ini photo_frames=: snapshots at these presents (sandbox, tests)
uint32_t g_capturePresents = 4; // the capture lasts several presents (frame generation: one in two without a game draw)
bool g_liveAtStart = false;      // urti.ini live=1: live RT effects from launch
uint32_t g_calibrationInterval = 180;
bool g_liveCapture = false;
std::atomic<uint32_t> g_presentDraws{ 0 }; // game draws since the last present (0: generated frame)
// draws executed by the game's queue: written in the same present window, or in an earlier one
std::atomic<uint64_t> g_presentIndex{ 0 };
// capture cost (log): CPU time in CaptureDraw (QueryPerformanceCounter units), draws seen, draws replayed and their vertices
// (measurement area included)
std::atomic<uint64_t> g_capTicks{ 0 }, g_capCalls{ 0 }, g_capReplayed{ 0 }, g_capReplayVerts{ 0 }, g_capProbeReplayed{ 0 }, g_capProbeVerts{ 0 };
std::atomic<uint64_t> g_capSunVerts{ 0 }, g_capTranspVerts{ 0 }, g_capMainVerts{ 0 }, g_capOtherVerts{ 0 }; // replayed vertices: sun map, transparent surfaces, main screen (measurement excluded), other screens
std::atomic<uint32_t> g_execOnTime{ 0 }, g_execLate{ 0 }, g_execLists{ 0 }, g_execLateLists{ 0 };
std::atomic<uint32_t> g_presentExecDraws{ 0 }; // draws executed since the last present (0: generated frame)
// frame-generation diagnostic: per queue, presents and presents preceded by executed draws;
// queues where the game executes its draws; sequence of presents (1: draws executed before it)
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_diagPresentQueues;
std::unordered_map<uint64_t, uint32_t> g_diagExecQueues;
uint64_t g_diagPattern = 0, g_diagBbPattern = 0;
// main-screen draws ignored by the capture: transparent surfaces (depth tested, not written),
// tessellation
uint64_t g_diagTranspDraws = 0, g_diagTranspVerts = 0, g_diagTranspMax = 0, g_diagTessDraws = 0, g_diagTessVerts = 0;
uint64_t g_diagBlendDraws = 0, g_diagBlendVerts = 0;
std::map<uint64_t, uint32_t> g_diagBlendSizes; // vertices of a blended draw -> number of draws
// screen images the game drew into in lists executed since the last present
std::unordered_set<uint64_t> g_execBackBuffers;
// the game's G-buffer: render targets bound with the screen depth (at least three). Among these sets of
// targets, the one the game draws into the most: in RDR2, a later pass (transparents) also binds five
// targets, two of them from the G-buffer; the last binding seen took it part of the time (smoothness lost)
uint64_t g_liveGbuffer[8] = {};
uint32_t g_liveGbufferCount = 0;
struct GbufferSet
{
    uint64_t targets[8] = {};
    uint32_t count = 0;
    uint64_t draws = 0;
};
std::unordered_map<uint64_t, GbufferSet> g_gbufferSets; // key: the first target
void LiveGbufferBarrier(command_list* cmd, uint32_t count, const resource* resources, const resource_usage* oldStates, const resource_usage* newStates);
// live mode: marked draws of the current frame. The sun's shadow map and the screen see the
// same meshes: pairs with the same signature give the sun direction every frame (GPU)
struct LiveMark
{
    uint64_t key = 0;
    uint32_t slot = 0;
    bool sunMap = false;
    float viewport[4] = {}; // in the shadow map: the draw's cascade
    uint32_t offset = 0, capacity = 0, counter = 0; // the draw's region in its slot's stream
};
// live, each draw has its region of the stream and its counter: no barrier between draws (the
// shared counter forced a GPU wait at every draw, thousands per frame)
// id: identity of the drawn object, the same from one frame to the next (draw signature and rank among the
// draws with the same signature: an engine often draws the same mesh in several places)
struct LiveRegion
{
    uint32_t offset = 0, capacity = 0, counter = 0, id = 0;
};
std::unordered_map<uint64_t, uint32_t> g_liveKeyRank; // screen draws of each signature executed in this frame
// Draws ordered the way the game's queue executes them, not the way they are written: some engines
// write a third of their command lists before the previous present (next frame prepared
// ahead); ordered at writing, their objects changed identity every frame (rank among draws with the same
// signature mixing two frames), their regions were read before being filled and their counters taken over by
// others. At writing, a captured draw receives a region in the part of the stream of its writing window (three
// rotating parts) and a counter of that part; when its list executes, its rank, its identity, its place
// in the RT scene and the present it belongs to
struct LivePending
{
    uint64_t rankKey = 0, key = 0, window = 0;
    uint32_t slot = 0, src = 0, capacity = 0, counter = 0, vertices = 0, sig = 0;
    uint32_t part = 0; // huge mesh captured in parts: part number + 1 (its identity does not take the other parts' place)
    // end-of-writing marker of the main depth (not a draw): buffer of its copy, draws of the list before it
    int32_t depthMark = -1;
    uint32_t mainBefore = 0, depthAfter = 0;
    int32_t materialMark = -1; // end-of-writing marker of the material image: buffer of its copy
    // end of writing of a screen-sized RGBA 16-bit float image (candidate for the UI-free image): the resource,
    // and the buffer of its copy if it is the adopted image
    uint64_t hudHandle = 0;
    int32_t hudMark = -1;
    uint64_t hudSerialMark = 0; // copy of the UI-free image executed (its number): marker left in the draw order, to know which game frame it belongs to
    bool screen = false, mainScreen = false, probe = false, sunMap = false, marked = false, indirect = false;
    // vertices in the same order from one frame to the next: a direct draw of a single copy (an ExecuteIndirect or
    // copies chosen on the GPU change order and count: their vertex pairs say nothing about the
    // camera's motion)
    bool stableOrder = false;
    bool transp = false; // surface transparente (couche 2)
    float viewport[4] = {};
    uint32_t camSerial = 0; // the game's current camera when this draw was written (urti_camscan.inl)
};
uint32_t g_liveCommitCam = 0; // the game's camera of the last submitted main-screen draw
constexpr uint32_t kLiveAreas = 4;
constexpr uint32_t kLiveAreaMaxAge = 3; // list executed this many windows or more after its writing: ignored
std::unordered_map<uint64_t, std::vector<LivePending>> g_livePending; // command list -> captured draws not executed yet
// One game frame at a time (urti.ini frame_sync=, with the game's camera). The game executes its lists on a different queue from the
// present queue: at a present, it may have started the next frame. The depth copied at that moment is half drawn
// (the effect weakens at once, one time in ten, still camera), and a capture window holds a frame and a half or two.
// The depth is therefore copied inside the game's list, at the end of its writing; that marker, at execution, closes the window: the rest
// (next frame) waits for the next computation.
struct LiveDeferred
{
    std::vector<LivePending> pending;
    uint32_t listMain = 0, counted = 0;
};
bool g_liveFrameSync = false, g_liveTraceOn = false;
bool g_liveWindowClosed = false;
std::vector<LiveDeferred> g_liveDeferred;
int32_t g_liveSnapDone = -1;   // buffer of the last complete depth
uint64_t g_liveSnapSerial = 0; // number of complete depths
uint32_t g_liveSnapNext = 0, g_liveMainSinceMark = 0, g_liveMainLast = 0, g_liveMarksRefused = 0;
// main-depth draws written, at the last copy: no new copy if nothing has been written since (the game changes
// its depth's state about ten times per frame, and six copies in a row took over the right one's buffer)
uint64_t g_liveMainRecorded = 0, g_liveMainAtCopy = ~0ull;
// material image (water mask) of the same game frame as the depth: copied separately, a bit before or a bit after it. With the
// previous frame's copy, in motion, a leaf passed for water (the sky's material, the leaf's depth): a reflection going
// anywhere, a bright point, never seen with the game paused
uint32_t g_liveMaterialNext = 0, g_liveMaterialSinceClose = 0;
int32_t g_liveMaterialDone = -1, g_liveWindowMaterial = -1;
uint32_t g_liveSnapCopies = 0;
// the game's image without its UI (with DLSS-G: the target frame generation receives, alpha = the UI's shape).
// Recognised at execution: the first screen-sized RGBA 16-bit float image finished after the frame's material
// (after the depth without material); adopted when it is the same resource at least 8 times out of 10. Then copied every frame
uint64_t g_liveHudHandle = 0;
std::unordered_map<uint64_t, uint32_t> g_liveHudVotes;
uint32_t g_liveHudVoteCount = 0, g_liveHudNext = 0, g_liveHudW = 0, g_liveHudH = 0;
bool g_liveHudArmed = false;
int32_t g_liveHudDone = -1;
uint64_t g_liveHudDonePresent = 0, g_liveHudSerial = 0; // present and number of the last executed copy
uint64_t g_livePairSnap = 0, g_livePairHud = 0;         // last known pair: complete depth (number) and UI-free copy of the same game frame
uint64_t g_liveWindow = 0;                                // present window (advanced by urti_live.inl)
uint32_t g_liveAreaCursor[4][kLiveAreas] = {}, g_liveAreaProbe[4][kLiveAreas] = {}; // bytes taken in each part (start, end)
uint32_t g_liveAreaCounters[kLiveAreas] = {};             // counters taken in each part
uint32_t g_liveStaleDraws = 0;                            // draws executed too late (part of the stream already reused), ignored
// signature of the mesh without the rank: budget and learned sizes are per mesh (all its copies)
uint32_t MeshId(uint64_t idKey)
{
    const uint64_t h = (idKey ^ (idKey >> 31)) * 0x9E3779B97F4A7C15ull;
    return uint32_t(h ^ (h >> 32)) | 1u;
}
std::unordered_map<uint32_t, uint32_t> g_liveIdMesh; // identity -> mesh (this frame)
// transparent surfaces of the main screen (water, glass; also smoke, particles, discarded afterwards by
// live_planes): captured separately, stored one after the other in their own layer of the RT scene, without a per-mesh budget
constexpr uint32_t kLiveTranspVertices = 1500000;   // vertices of the layer (per-frame cap)
std::vector<LiveRegion> g_liveTranspRegions;        // execution order: start in the layer, size, counter
std::vector<uint32_t> g_liveTranspSrc;              // same order: start of the written region in the stream
uint32_t g_liveTranspCursor = 0;                    // bytes of the layer (16 per vertex)
uint64_t g_liveTranspVerts = 0;                     // vertices reserved in this writing window
uint32_t g_liveTranspRefused = 0;                   // draws refused (indirect, layer full)
uint32_t g_liveTraceVertices = 0;                   // urti.ini trace_draw=: log of every draw of this size
int g_liveMainSlotIndex = -1;                       // main-screen slot (draws without depth)
struct CensusEntry { uint64_t draws = 0, vertices = 0; std::map<uint32_t, uint32_t> sizes; };
std::map<uint32_t, CensusEntry> g_liveCensus;       // urti.ini motion_log=1: every draw of the scene, by class
bool g_liveCensusOn = false;
bool g_liveTranspOn = true;                         // urti.ini water=0: no transparent layer
bool g_liveTessOn = false;                          // urti.ini tess=1: tessellated main-screen draws captured (Red Dead's terrain)
// area where the game really draws its image inside the screen depth: the most frequent among the viewports
// of its draws (right edge, bottom edge), this window. With DLSS and dynamic resolution, a game may
// draw at 1280×720 to 1707×960 inside 2560×1440 targets; a few draws (3D markers)
// keep the full area there
std::unordered_map<uint32_t, uint32_t> g_liveViewCounts; // (width << 16 | height) -> draws
// geometry budget (urti_live.inl updates it every frame): an object already measured and of little importance
// (small, distant) does not enter the RT scene; those to re-measure are captured in the measurement area, at
// the end of the slot, outside the acceleration structure
std::vector<LiveRegion> g_liveProbes[4];
std::unordered_set<uint32_t> g_liveKnown, g_liveAdmit, g_liveProbeWant; // maillages
std::unordered_map<uint32_t, uint32_t> g_liveSeen; // screen meshes in this frame -> vertices (all copies)
// ExecuteIndirect: vertex count unknown before the draw; 65,536 vertices used to be reserved for each (empty
// room taken in the slot and in the structure). Size learned per mesh (measured), margin included.
std::unordered_map<uint32_t, uint32_t> g_liveLearned;
// in RDR2 the order of ExecuteIndirects changes from one frame to the next (unstable identity): the size is
// also learned per command signature (the largest number of vertices written by its draws)
std::unordered_map<uint32_t, uint32_t> g_liveLearnedSig, g_liveIdSig; // signature -> vertices; identity -> signature (this frame)
uint64_t g_liveReserved = 0; // vertices reserved in the main screen (summary)
uint64_t g_liveBudgetVertices = 0, g_liveProbeBudget = 0, g_liveProbeVerts = 0; // budget, measurement area (per frame)
std::unordered_map<uint32_t, uint32_t> g_liveProbeStride, g_liveProbeDraws, g_liveProbePhase; // huge mesh: one draw in N captured each round, this round's share
std::unordered_set<uint32_t> g_liveStableNow; // meshes whose vertices keep their order (direct draw, one instance)
std::unordered_set<uint64_t> g_liveExecQueues; // queues where game draws were executed since the last computation
// screen snapshots at the first draw of each binding of the screen image (urti_live.inl picks the one
// before the game's UI: reflections do not pick up menus there, the composition spares the UI)
constexpr uint32_t kLiveSnaps = 4;
std::unordered_set<uint64_t> g_liveBackBuffers;
ID3D12Resource* g_liveSnap[kLiveSnaps] = {};
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_liveSnapFp = {};
uint32_t g_liveSnapCount = 0;
bool g_liveBudget = false;
uint64_t g_liveMainDepth = 0; // main-screen depth (the one whose stream makes the RT scene): the only one under the budget
uint64_t g_liveSlotLimit = 0; // test (urti.ini slot_limit=, vertices): smaller slot, to reproduce saturation
uint32_t g_liveOverflow = 0, g_liveOutOfBudget = 0;
std::vector<LiveRegion> g_liveRegions[4]; // per slot (kSlots), in execution order: place in the RT scene
std::vector<uint32_t> g_liveRegionSrc[4]; // same order: start of the written region in the stream (bytes)
uint32_t g_liveCursor[4] = {};            // without fixed places: regions one after the other in the RT scene (bytes)
// fixed place of each main-screen object in its slot, from one frame to the next: an object the
// game skips for a few frames keeps its last geometry in the RT scene (no more flashes), and
// the stable layout lets the structure be updated instead of rebuilt (urti.ini persist=0:
// regions assigned one after the other every frame, as before)
struct LivePlace
{
    uint32_t offset = 0, capacity = 0, countIndex = 0, frameCounter = 0;
    uint64_t lastFrame = 0, prevFrame = 0; // last frame where the object was drawn, and the one before
    bool stableOrder = false;              // its vertices keep their order (pairs for the camera's motion)
    // mesh drawn only once in the previous frame: the place is indeed the same object (some games sort their
    // draws front to back: the n-th copy of a mesh is not the same from one frame to the next)
    bool uniquePrev = false;
    uint64_t rankKey = 0;
};
bool g_livePersist = true;
std::unordered_map<uint32_t, LivePlace> g_livePlaces; // identity -> place
std::map<uint32_t, uint32_t> g_liveFreeSpace;         // start -> size (bytes, multiples of 48: three vertices)
std::vector<uint32_t> g_liveFreeCounts;               // count cells given back
uint32_t g_livePlaceHigh = 0, g_liveNextCount = 0;    // end of the used part (bytes); count cells assigned
uint64_t g_livePlaceFrame = 1;                         // capture frame (advanced by urti_live.inl)
uint32_t g_livePlaceNew = 0, g_livePlaceFailed = 0;    // places assigned, refused (slot full) in this frame

// gives the place back: its space (merged with its free neighbours) and its count cell
void LiveFreePlace(const LivePlace& p)
{
    uint32_t start = p.offset, size = p.capacity;
    if (auto next = g_liveFreeSpace.find(start + size); next != g_liveFreeSpace.end())
        size += next->second, g_liveFreeSpace.erase(next);
    if (auto prev = g_liveFreeSpace.lower_bound(start); prev != g_liveFreeSpace.begin())
        if (--prev; prev->first + prev->second == start)
            start = prev->first, size += prev->second, g_liveFreeSpace.erase(prev);
    if (start + size == g_livePlaceHigh)
        g_livePlaceHigh = start; // at the top of the used part: it shrinks
    else
        g_liveFreeSpace[start] = size;
    g_liveFreeCounts.push_back(p.countIndex);
}
bool g_liveCombine = true;       // urti.ini combine=0: RT shadows alone, ignoring the game's
uint32_t g_liveDebugGame = 0;    // urti.ini debug_game_shadow=1: shows the recomputed game shadow
float g_liveGameBias = 0.0015f;  // urti.ini game_bias=: tolerance of the game's shadow-map test
float g_liveRayBias = 0.003f;    // urti.ini ray_bias=: ray start shifted toward the sun (share of the distance)
float g_liveSunRadius = 0.5f;    // urti.ini sun_radius= (degrees): apparent size of the sun, softens the edges
std::vector<LiveMark> g_liveMarks;
std::unordered_set<uint64_t> g_liveSunKeys, g_livePrevSunKeys;
uint64_t g_liveSunDepth = 0; // depth of the sun's shadow map (found at calibration)
uint32_t g_liveSunMapDesc[3] = {}; // width, height, format of that map (stored calibration)

uint32_t g_liveSunMarked = 0;
bool g_liveSunEighth = false; // sun shadows off: sun map captured in eighths
bool g_liveSunUnused = false; // no sun shadows, occlusion or bounce (menu): the sun map is no longer replayed, the sun no longer searched for
constexpr uint32_t kLiveMaxSunMarks = 1024, kLiveMaxMarks = 4096;
bool g_liveShowMask = false;     // urti.ini show_mask=1: the effect shows the RT shadows alone      // live mode captures the screen depth every frame

void Log(const char* fmt, ...)
{
    char text[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (FILE* f = std::fopen(g_logPath.c_str(), "a"))
    {
        std::fputs(text, f);
        std::fputc('\n', f);
        std::fclose(f);
    }
    reshade::log::message(reshade::log::level::info, text);
}

void ReadSettings(HMODULE module)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    std::string self = path;
    g_dir = self.substr(0, self.find_last_of("\\/"));
    if (const char* out = std::getenv("URTI_OUT"))
        g_dir = out;
    g_logPath = g_dir + "\\urti_log.txt";

    auto parseList = [](const std::string& s) {
        std::vector<uint64_t> v;
        size_t i = 0;
        while (i < s.size())
        {
            const size_t j = s.find(',', i);
            const std::string part = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (!part.empty())
            {
                // "a-b": every value from a to b
                char* end = nullptr;
                const uint64_t a = std::strtoull(part.c_str(), &end, 10);
                const uint64_t b = (end && *end == '-') ? std::strtoull(end + 1, nullptr, 10) : a;
                for (uint64_t x = a; x <= b && x < a + 10000; x++)
                    v.push_back(x);
            }
            if (j == std::string::npos)
                break;
            i = j + 1;
        }
        return v;
    };
    if (const char* f = std::getenv("URTI_CAPTURE_FRAME"))
        g_captureFrames = parseList(f);
    if (FILE* f = std::fopen((self.substr(0, self.find_last_of("\\/")) + "\\urti.ini").c_str(), "r"))
    {
        char line[512];
        while (std::fgets(line, sizeof(line), f))
        {
            std::string l = line;
            l.erase(l.find_last_not_of(" \r\n\t") + 1);
            if (l.rfind("capture_frames=", 0) == 0)
                g_captureFrames = parseList(l.substr(15));
            if (l.rfind("photo_frames=", 0) == 0)
                g_photoFrames = parseList(l.substr(13));
            if (l.rfind("show_mask=", 0) == 0)
                g_liveShowMask = std::atoi(l.c_str() + 10) != 0;
            if (l.rfind("combine=", 0) == 0)
                g_liveCombine = std::atoi(l.c_str() + 8) != 0;
            if (l.rfind("debug_game_shadow=", 0) == 0)
                g_liveDebugGame = uint32_t(std::atoi(l.c_str() + 18));
            if (l.rfind("live=", 0) == 0)
                g_liveAtStart = std::atoi(l.c_str() + 5) != 0;
            if (l.rfind("calibration_interval=", 0) == 0)
                g_calibrationInterval = std::max(30, std::atoi(l.c_str() + 21));
            if (l.rfind("capture_presents=", 0) == 0)
                g_capturePresents = std::max(1u, (uint32_t)std::strtoul(l.c_str() + 17, nullptr, 10));
        }
        std::fclose(f);
    }
    std::sort(g_captureFrames.begin(), g_captureFrames.end());
    std::sort(g_photoFrames.begin(), g_photoFrames.end());
}

// ------------------------------------------------------------------------------------------------
// root signatures: ALLOW_STREAM_OUTPUT added
// ------------------------------------------------------------------------------------------------

using PFN_CreateRootSignature = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
PFN_CreateRootSignature g_createRootSignature = nullptr;
std::atomic<int> g_rootSignaturesPatched{ 0 };

HRESULT STDMETHODCALLTYPE HookCreateRootSignature(ID3D12Device* self, UINT nodeMask, const void* blob, SIZE_T size, REFIID riid, void** out)
{
    ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&deserializer))) &&
        (desc = deserializer->GetUnconvertedRootSignatureDesc()) != nullptr)
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC patched = *desc;
        if (patched.Version == D3D_ROOT_SIGNATURE_VERSION_1_0)
            patched.Desc_1_0.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
        else
            patched.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
        ComPtr<ID3DBlob> serialized, errors;
        if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&patched, &serialized, &errors)))
        {
            const HRESULT hr = g_createRootSignature(self, nodeMask, serialized->GetBufferPointer(), serialized->GetBufferSize(), riid, out);
            if (SUCCEEDED(hr))
            {
                g_rootSignaturesPatched++;
                return hr;
            }
        }
    }
    return g_createRootSignature(self, nodeMask, blob, size, riid, out);
}

// replaced function-table entries, restored when the add-on unloads: ReShade
// unloads and reloads add-ons for every device (RDR2 creates several at startup to test the
// graphics card); a table still pointing to an unloaded add-on's code would crash the
// game at the first call (ucrtbase, 0xc0000409, without OptiScaler which loaded ReShade only once)
struct VtablePatch
{
    void** slot;
    void* original;
    void* hook;
};
std::vector<VtablePatch> g_vtablePatches;

void PatchVtable(void* object, int index, void* hook, void** original, const char* name)
{
    void** vtable = *reinterpret_cast<void***>(object);
    if (vtable[index] == hook)
        return;
    DWORD old = 0;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    if (!*original)
        *original = vtable[index];
    g_vtablePatches.push_back({ &vtable[index], vtable[index], hook });
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    Log("URTI: %s hooked", name);
}

void UnpatchVtables()
{
    for (auto it = g_vtablePatches.rbegin(); it != g_vtablePatches.rend(); ++it)
    {
        if (*it->slot != it->hook)
            continue;
        DWORD old = 0;
        VirtualProtect(it->slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        *it->slot = it->original;
        VirtualProtect(it->slot, sizeof(void*), old, &old);
    }
    g_vtablePatches.clear();
}

// ------------------------------------------------------------------------------------------------
// the game's pipelines and "shadow" pipelines
// ------------------------------------------------------------------------------------------------

struct PipelineInfo
{
    std::vector<uint8_t> vs;
    std::vector<std::string> semantics;
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    ID3D12RootSignature* rootSignature = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    bool usable = false; // vertex shader only (no geometry / tessellation / mesh)
    bool writesDepth = true; // deferred lighting passes, decals, transparents: read-only depth
    bool depthTested = false;   // depth tested (a transparent surface: water, glass, but also smoke, decals)
    bool tessellated = false;   // tessellation (water waves in some engines)
    bool blends = false;        // blended into the image on the first target (transparent surface: water, glass, smoke)
    bool transparentUsable = false; // transparent surface captured separately (layer 2 of the RT scene: water reflections)
    bool noDepthUsable = false;     // triangles without depth test (surface drawn without a bound depth)
    bool otherStage = false;        // geometry, tessellation or mesh shader
    std::vector<uint8_t> hs, ds;    // tessellation: replayed with it (Red Dead's terrain), output after the domain shader
    bool gsOrMesh = false;
    bool tessUsable = false;        // tessellation only (neither geometry nor mesh), writes depth
    ComPtr<ID3D12PipelineState> shadow;
    bool shadowFailed = false;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, PipelineInfo> g_pipelines;
std::atomic<uint32_t> g_pipelinesSeen{ 0 }, g_pipelinesUsable{ 0 }, g_shadowFailures{ 0 };

void OnInitPipeline(device* dev, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe)
{
    if (dev->get_api() != device_api::d3d12)
        return;
    PipelineInfo info;
    bool hasVs = false, other = false;
    for (uint32_t i = 0; i < count; i++)
    {
        const pipeline_subobject& s = subobjects[i];
        switch (s.type)
        {
        case pipeline_subobject_type::vertex_shader:
        {
            const shader_desc* d = static_cast<const shader_desc*>(s.data);
            if (d && d->code && d->code_size)
            {
                info.vs.assign(static_cast<const uint8_t*>(d->code), static_cast<const uint8_t*>(d->code) + d->code_size);
                hasVs = true;
            }
            break;
        }
        case pipeline_subobject_type::hull_shader:
        case pipeline_subobject_type::domain_shader:
            if (const shader_desc* d = static_cast<const shader_desc*>(s.data); d && d->code && d->code_size)
            {
                other = true, info.tessellated = true;
                (s.type == pipeline_subobject_type::hull_shader ? info.hs : info.ds)
                    .assign(static_cast<const uint8_t*>(d->code), static_cast<const uint8_t*>(d->code) + d->code_size);
            }
            break;
        case pipeline_subobject_type::geometry_shader:
        case pipeline_subobject_type::mesh_shader:
        case pipeline_subobject_type::amplification_shader:
        case pipeline_subobject_type::compute_shader:
            if (s.data && static_cast<const shader_desc*>(s.data)->code_size)
                other = true, info.gsOrMesh = true;
            break;
        case pipeline_subobject_type::input_layout:
        {
            const input_element* e = static_cast<const input_element*>(s.data);
            for (uint32_t k = 0; k < s.count; k++)
            {
                info.semantics.push_back(e[k].semantic ? e[k].semantic : "");
                D3D12_INPUT_ELEMENT_DESC d = {};
                d.SemanticIndex = e[k].semantic_index;
                d.Format = static_cast<DXGI_FORMAT>(e[k].format); // ReShade's formats follow DXGI
                d.InputSlot = e[k].buffer_binding;
                d.AlignedByteOffset = e[k].offset;
                d.InputSlotClass = e[k].instance_step_rate ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                d.InstanceDataStepRate = e[k].instance_step_rate;
                info.layout.push_back(d);
            }
            break;
        }
        case pipeline_subobject_type::depth_stencil_state:
        {
            const depth_stencil_desc* d = static_cast<const depth_stencil_desc*>(s.data);
            if (d)
                info.writesDepth = d->depth_enable && d->depth_write_mask, info.depthTested = d->depth_enable;
            break;
        }
        case pipeline_subobject_type::blend_state:
        {
            const blend_desc* d = static_cast<const blend_desc*>(s.data);
            if (d)
                info.blends = d->blend_enable[0];
            break;
        }
        case pipeline_subobject_type::primitive_topology:
        {
            const primitive_topology t = *static_cast<const primitive_topology*>(s.data);
            if (t == primitive_topology::point_list)
                info.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            else if (t == primitive_topology::line_list || t == primitive_topology::line_strip)
                info.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            else if (t >= primitive_topology::patch_list_01_cp)
                other = true, info.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            break;
        }
        default:
            break;
        }
    }
    if (!hasVs)
        return;
    g_pipelinesSeen++;
    info.rootSignature = reinterpret_cast<ID3D12RootSignature*>(layout.handle);
    // only geometry that writes depth counts: a full-screen pass reading it does not describe the scene
    info.usable = !other && info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && info.rootSignature && info.writesDepth;
    if (info.usable)
        g_pipelinesUsable++;
    // (blended or not: some water computes its refraction itself and writes its colour in one block;
    // draws writing into the G-buffer are discarded at capture)
    info.transparentUsable = !other && info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && info.rootSignature && info.depthTested && !info.writesDepth;
    info.noDepthUsable = !other && info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && info.rootSignature && !info.depthTested;
    info.otherStage = other;
    info.tessUsable = info.tessellated && !info.gsOrMesh && !info.hs.empty() && !info.ds.empty() && info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH &&
                      info.rootSignature && info.writesDepth;
    std::lock_guard<std::mutex> lock(g_mutex);
    PipelineInfo& stored = g_pipelines[pipe.handle] = std::move(info);
    for (size_t k = 0; k < stored.layout.size(); k++)
        stored.layout[k].SemanticName = stored.semantics[k].c_str(); // after the move
}

void OnDestroyPipeline(device*, pipeline pipe)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pipelines.erase(pipe.handle);
}

ID3D12PipelineState* GetShadow(ID3D12Device* device, PipelineInfo& info)
{
    if (info.shadow || info.shadowFailed)
        return info.shadow.Get();
    static const D3D12_SO_DECLARATION_ENTRY entry = { 0, "SV_Position", 0, 0, 4, 0 };
    static const UINT stride = 16;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = info.rootSignature;
    desc.VS = { info.vs.data(), info.vs.size() };
    if (info.tessUsable)
        desc.HS = { info.hs.data(), info.hs.size() }, desc.DS = { info.ds.data(), info.ds.size() }; // output: triangles after tessellation
    desc.StreamOutput.pSODeclaration = &entry;
    desc.StreamOutput.NumEntries = 1;
    desc.StreamOutput.pBufferStrides = &stride;
    desc.StreamOutput.NumStrides = 1;
    desc.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.InputLayout = { info.layout.data(), (UINT)info.layout.size() };
    desc.PrimitiveTopologyType = info.topology;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&info.shadow))))
    {
        info.shadowFailed = true;
        if (g_shadowFailures++ < 10)
            Log("URTI: shadow pipeline refused (vertex shader of %zu bytes)", info.vs.size());
    }
    return info.shadow.Get();
}

// ------------------------------------------------------------------------------------------------
// capture
// ------------------------------------------------------------------------------------------------

// One stream per depth (the screen, the shadow maps): all the
// draws of a stream share its counter, and D3D12 stores their outputs one after the other, at their
// exact size, in GPU execution order
constexpr uint64_t kSoBytes = 512ull << 20; // 32 M sommets
constexpr uint32_t kSlots = 4; // the screen and up to three shadow maps (sun direction)
constexpr uint64_t kSlotBytes = kSoBytes / kSlots;
constexpr uint32_t kMaxDraws = 65536;
// live: three rotating parts per slot (one per writing window), counters per part
constexpr uint64_t kLiveAreaBytes = kSlotBytes / kLiveAreas / 48 * 48;
constexpr uint32_t kLiveAreaCounters = (kMaxDraws - kSlots) / kLiveAreas;

// a captured draw: signature (to find it from one frame to the next) and place in its stream
struct DrawRecord
{
    uint32_t slot = 0, frame = 0, kind = 0; // kind: 0 direct, 1 indexed, 2 indirect
    uint32_t params[5] = {};                 // draw parameters (counts, first indices…)
    uint64_t pipeline = 0;
};

struct CommandListState
{
    uint64_t pipeline = 0;
    uint64_t depth = 0;
    uint64_t indexBuffer = 0, vertexBuffer = 0; // bound buffers (resource and offset mixed): identity of ExecuteIndirects
    uint64_t backBuffer = 0; // screen image bound as a target (snapshots before the UI at its first draws)
    uint32_t backBufferDraws = 0; // draws into the screen image since its binding
    uint64_t gbufferSet = 0; // bound G-buffer candidate set of targets (its first target), 0: none
    float viewport[4] = {}; // x, y, width, height (the first one)
    // present during which the list received its first draw, draws since its reset: a list
    // written before a present and executed after it (engines that prepare the next frame ahead) is seen
    uint64_t window = ~0ull;
    uint32_t draws = 0;
    uint64_t drewBackBuffer = 0; // screen image this list drew into (real or generated frame: see OnPresent)
    uint32_t mainDraws = 0;      // draws into the main depth written in this list
};

enum class Phase
{
    Idle,
    Capturing,    // frame being captured
    WaitingDepth, // next frame: depth copied at its clear
};

struct Capture
{
    ID3D12Device* device = nullptr; // the one that presents
    ComPtr<ID3D12Resource> so, counters, zeros, soReadback, counterReadback, depthReadback, marks, marksReadback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;

    uint64_t frame = 0, nextCapture = ~0ull;
    size_t captureListIndex = 0;
    std::atomic<Phase> phase{ Phase::Idle };
    std::atomic<uint64_t> soUsed{ 0 };
    std::atomic<uint32_t> drawCount{ 0 }, frameDraws{ 0 }, skippedDraws{ 0 }, overflowDraws{ 0 };
    std::vector<DrawRecord> draws;
    std::unordered_map<uint64_t, CommandListState> lists;
    std::unordered_map<uint64_t, uint32_t> depthStates; // last known state (barriers)

    uint64_t depthResource = 0;
    std::atomic<bool> depthCopied{ false };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint = {};
    D3D12_RESOURCE_DESC depthDesc = {};
    uint32_t captureDrawCount = 0, captureIndex = 0, presentsLeft = 0, depthWaits = 0;
    uint64_t slotDepth[kSlots] = {};
    std::atomic<uint32_t> slotDraws[kSlots];
    float slotScore[kSlots] = {}; // draws of the previous frames (running average): choice of the main screen
    std::atomic<uint32_t> offScreenDraws{ 0 };
    uint32_t screenW = 0, screenH = 0;
    std::unordered_map<uint64_t, int> screenLike; // 1: screen-sized depth, 0: other, -1: unusable
    int captureSlot = -1;
    bool depthReversed = false;
    bool liveCalibration = false; // live-mode calibration capture: nothing is written to disk
    std::unordered_map<uint64_t, uint32_t> drawsPerDevice; // diagnostic: draws of every device during the capture
} g;

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = { heapType };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

bool InitResources(ID3D12Device* device)
{
    g.so = MakeBuffer(device, kSoBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
    g.counters = MakeBuffer(device, kMaxDraws * 8ull, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
    g.zeros = MakeBuffer(device, kMaxDraws * 8ull, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g.soReadback = MakeBuffer(device, kSoBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    g.counterReadback = MakeBuffer(device, kMaxDraws * 8ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    g.marks = MakeBuffer(device, kMaxDraws * 16ull, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    g.marksReadback = MakeBuffer(device, kMaxDraws * 16ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g.so || !g.counters || !g.zeros || !g.soReadback || !g.counterReadback || !g.marks || !g.marksReadback)
        return false;
    void* p = nullptr;
    if (FAILED(g.zeros->Map(0, nullptr, &p)))
        return false;
    std::memset(p, 0, kMaxDraws * 8ull);
    g.zeros->Unmap(0, nullptr);
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        return false;
    g.list->Close();
    g.draws.resize(kMaxDraws);
    g.device = device;
    return true;
}

bool OurDevice(device* dev)
{
    return dev->get_api() == device_api::d3d12 && g.device && dev->get_native() == reinterpret_cast<uint64_t>(g.device);
}

void CamReaderInstall(void* device); // urti_camscan.inl
void OnInitDevice(device* dev)
{
    if (dev->get_api() != device_api::d3d12)
        return;
    PatchVtable(reinterpret_cast<void*>(dev->get_native()), 16, reinterpret_cast<void*>(&HookCreateRootSignature),
                reinterpret_cast<void**>(&g_createRootSignature), "CreateRootSignature");
    CamReaderInstall(reinterpret_cast<void*>(dev->get_native()));
}

void NrdDestroy(); // urti_nrd.inl

void OnDestroyDevice(device* dev)
{
    if (!OurDevice(dev))
        return;
    NrdDestroy();
    g.phase = Phase::Idle;
    g.so.Reset(), g.counters.Reset(), g.zeros.Reset(), g.soReadback.Reset(), g.counterReadback.Reset(), g.depthReadback.Reset();
    g.list.Reset(), g.allocator.Reset(), g.fence.Reset();
    g.device = nullptr;
}

ID3D12GraphicsCommandList* Native(command_list* cmd)
{
    return reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
}

void OnBindPipeline(command_list* cmd, pipeline_stage stages, pipeline pipe)
{
    if ((stages & pipeline_stage::vertex_shader) == 0 && stages != pipeline_stage::all)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.lists[cmd->get_native()].pipeline = pipe.handle;
}

// bound index and vertex buffers: what stays stable from one frame to the next for an ExecuteIndirect (its
// argument buffer rotates in RDR2: an identity depending on it changes every frame)
void OnBindIndexBuffer(command_list* cmd, resource buffer, uint64_t offset, uint32_t)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g.lists[cmd->get_native()].indexBuffer = buffer.handle ^ (offset * 0x9E3779B97F4A7C15ull);
}

void OnBindVertexBuffers(command_list* cmd, uint32_t first, uint32_t count, const resource* buffers, const uint64_t* offsets, const uint32_t*)
{
    if (first != 0 || count == 0 || !buffers)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.lists[cmd->get_native()].vertexBuffer = buffers[0].handle ^ ((offsets ? offsets[0] : 0) * 0x9E3779B97F4A7C15ull);
}

// pass survey (urti_passes.inl)
void PassBind(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view dsv);
void PassDraw(uint64_t list, bool fullscreen);
void PassBarrier(command_list* cmd, uint32_t count, const resource* resources, const resource_usage* oldStates, const resource_usage* newStates);

void OnBindRenderTargets(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view dsv)
{
    PassBind(cmd, count, rtvs, dsv);
    const uint64_t depth = dsv.handle ? cmd->get_device()->get_resource_from_view(dsv).handle : 0;
    uint64_t targets[8] = {};
    if (g_liveCapture && depth && count >= 3)
        for (uint32_t i = 0; i < count && i < 8; i++)
            targets[i] = rtvs[i].handle ? cmd->get_device()->get_resource_from_view(rtvs[i]).handle : 0;
    const uint64_t first = count && rtvs[0].handle && !g_liveBackBuffers.empty() ? cmd->get_device()->get_resource_from_view(rtvs[0]).handle : 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    CommandListState& st = g.lists[cmd->get_native()];
    st.depth = depth;
    st.backBuffer = first && g_liveBackBuffers.count(first) ? first : 0;
    st.backBufferDraws = 0;
    st.gbufferSet = 0;
    if (targets[0])
        if (auto it = g.screenLike.find(depth); it != g.screenLike.end() && it->second == 1)
        {
            // the same first target bound with fewer targets (in RDR2, three instead of five for part
            // of the objects): the widest binding is kept (otherwise the motion vectors, fifth, were lost)
            GbufferSet& set = g_gbufferSets[targets[0]];
            if (std::min<uint32_t>(count, 8) >= set.count)
            {
                std::memcpy(set.targets, targets, sizeof(targets));
                set.count = std::min<uint32_t>(count, 8);
            }
            st.gbufferSet = targets[0];
        }
}


#include "urti_camscan.inl"
uint32_t g_liveCamSerialNow() { return g_camSerial.load(std::memory_order_acquire); }

// initial state of depths (a shadow map may stay in write state without ever a barrier)
void OnInitResource(device*, const resource_desc& desc, const subresource_data*, resource_usage initialState, resource res)
{
    CamScanNoteResource(desc, res);
    if ((desc.usage & resource_usage::depth_stencil) == 0)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.depthStates.emplace(res.handle, static_cast<uint32_t>(initialState));
}

void OnBindViewports(command_list* cmd, uint32_t first, uint32_t count, const viewport* viewports)
{
    if (first != 0 || !count)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    float* v = g.lists[cmd->get_native()].viewport;
    v[0] = viewports[0].x, v[1] = viewports[0].y, v[2] = viewports[0].width, v[3] = viewports[0].height;
}

void OnBarrier(command_list* cmd, uint32_t count, const resource* resources, const resource_usage* oldStates, const resource_usage* newStates)
{
    PassBarrier(cmd, count, resources, oldStates, newStates);
    LiveGbufferBarrier(cmd, count, resources, oldStates, newStates);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < count; i++)
        if (g.depthStates.count(resources[i].handle) || (static_cast<uint32_t>(newStates[i]) & 0x30))
            g.depthStates[resources[i].handle] = static_cast<uint32_t>(newStates[i]);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

// stream of the depth (created at first encounter); -1: off-screen depth or no room left
int SlotFor(uint64_t depth)
{
    if (!depth)
        return -1;
    auto it = g.screenLike.find(depth);
    if (it == g.screenLike.end())
    {
        const D3D12_RESOURCE_DESC d = reinterpret_cast<ID3D12Resource*>(depth)->GetDesc();
        int kind = -1; // -1 unusable, 0 other (shadow maps…), 1 screen size
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Height && d.SampleDesc.Count == 1)
        {
            kind = 0;
            if (g.screenW && g.screenH)
            {
                const double a = double(d.Width) / d.Height, s = double(g.screenW) / g.screenH;
                if (std::fabs(a - s) < 0.05 * s && d.Width >= 0.3 * g.screenW)
                    kind = 1;
            }
        }
        it = g.screenLike.emplace(depth, kind).first;
        // stored calibration: the sun's shadow map is the depth with the same description
        if (!g_liveSunDepth && g_liveSunMapDesc[0] && kind == 0 && uint32_t(d.Width) == g_liveSunMapDesc[0] && d.Height == g_liveSunMapDesc[1] &&
            uint32_t(d.Format) == g_liveSunMapDesc[2])
        {
            g_liveSunDepth = depth;
            Log("URTI live: sun shadow map recognised (%ux%u format %u)", g_liveSunMapDesc[0], g_liveSunMapDesc[1], g_liveSunMapDesc[2]);
        }
    }
    if (it->second < 0)
        return -1;
    // live, the slots only serve the screen and the sun's shadow map, which has the
    // last one, reserved (otherwise several screen-sized depths take them all)
    if (g.phase != Phase::Capturing)
    {
        if (depth == g_liveSunDepth)
        {
            g.slotDepth[kSlots - 1] = depth;
            return int(kSlots - 1);
        }
        if (it->second != 1)
            return -1;
        for (uint32_t s = 0; s + 1 < kSlots; s++)
        {
            if (g.slotDepth[s] == depth)
                return int(s);
            if (!g.slotDepth[s] || g.slotDepth[s] == g_liveSunDepth)
            {
                g.slotDepth[s] = depth;
                return int(s);
            }
        }
        // slots full: the one used least in this frame is taken over
        uint32_t least = 0;
        for (uint32_t s = 1; s + 1 < kSlots; s++)
            if (g.slotDraws[s] < g.slotDraws[least])
                least = s;
        g.slotDepth[least] = depth;
        g.slotScore[least] = 0.0f;
        return int(least);
    }
    for (uint32_t s = 0; s < kSlots; s++)
    {
        if (g.slotDepth[s] == depth)
            return int(s);
        if (!g.slotDepth[s])
        {
            g.slotDepth[s] = depth;
            return int(s);
        }
    }
    return -1;
}

// replays the draw with the shadow pipeline; the game's draw happens afterwards, unchanged
void CaptureDrawImpl(ID3D12GraphicsCommandList* list, uint32_t kind, const uint32_t (&params)[5], const std::function<void(ID3D12GraphicsCommandList*)>& issue);

void CaptureDraw(ID3D12GraphicsCommandList* list, uint32_t kind, const uint32_t (&params)[5], const std::function<void(ID3D12GraphicsCommandList*)>& issue)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    CaptureDrawImpl(list, kind, params, issue);
    QueryPerformanceCounter(&t1);
    g_capTicks.fetch_add(uint64_t(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
    g_capCalls.fetch_add(1, std::memory_order_relaxed);
}

void CaptureDrawImpl(ID3D12GraphicsCommandList* list, uint32_t kind, const uint32_t (&params)[5], const std::function<void(ID3D12GraphicsCommandList*)>& issue)
{
    g.frameDraws++;
    CommandListState state;
    ID3D12PipelineState* shadow = nullptr;
    int slot = -1;
    bool transp = false; // transparent surface (water, glass…): separate layer of the RT scene
    bool tess = false;   // tessellated draw (terrain): output size unknown in advance, learned
    bool noDepth = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        state = g.lists[(uint64_t)list];
        if (state.gbufferSet)
            if (auto set = g_gbufferSets.find(state.gbufferSet); set != g_gbufferSets.end())
                set->second.draws++;
        slot = SlotFor(state.depth);
        auto it = g_pipelines.find(state.pipeline);
        // transparent surface (layer 2 of the RT scene): on the main depth, depth tested without being
        // written, outside the chosen G-buffer (the main pass of an engine with a depth pre-pass redraws
        // the whole scene that way into the G-buffer); or any draw on another view of the camera (same shape as the screen,
        // reduced resolution): some games draw their water at half resolution, in their own images and their
        // own depth. live_planes keeps only flat surfaces; live_reflect, only those with some thickness
        // above what the depth shows (not decals laid on the ground)
        const bool liveTransp = g.phase != Phase::Capturing && g_liveTranspOn && g_liveMainDepth;
        const bool otherView = liveTransp && state.depth != g_liveMainDepth && g.screenLike[state.depth] == 1;
        if (g_liveCensusOn && g.phase != Phase::Capturing)
        {
            const uint32_t depthKind = !state.depth ? 0 : state.depth == g_liveMainDepth ? 1 : g.screenLike[state.depth] == 1 ? 2 : 3;
            uint32_t cls = 7, blended = 0;
            if (it != g_pipelines.end())
            {
                cls = it->second.usable ? 0 : it->second.transparentUsable ? 1 : it->second.noDepthUsable ? 2 : it->second.tessellated ? 3 : it->second.otherStage ? 4 : 5;
                blended = it->second.blends ? 1 : 0;
            }
            CensusEntry& ce = g_liveCensus[(depthKind << 8) | (cls << 4) | (blended << 1) | (kind == 2 ? 1 : 0)];
            const uint32_t v = kind == 2 ? 0 : params[0] * std::max(1u, params[1]);
            ce.draws++, ce.vertices += v;
            if (kind != 2)
                ce.sizes[v]++;
        }
        // (draws without depth in some games are haze clouds: not captured for now)
        if (g_liveTraceVertices && kind != 2 && params[0] * std::max(1u, params[1]) == g_liveTraceVertices)
            Log("TRACE draw %u: kind %u depth %llx (main %llx) shape %d stream %d usable %d transparent %d phase %d window %llu", g_liveTraceVertices, kind, (unsigned long long)state.depth,
                (unsigned long long)g_liveMainDepth, state.depth ? g.screenLike[state.depth] : -9, slot, it != g_pipelines.end() ? int(it->second.usable) : -1,
                it != g_pipelines.end() ? int(it->second.transparentUsable) : -1, int(g.phase.load()), (unsigned long long)g_liveWindow);
        if (slot >= 0 && it != g_pipelines.end())
        {
            if (noDepth)
                shadow = GetShadow(g.device, it->second), transp = true;
            else if (otherView && it->second.transparentUsable)
                shadow = GetShadow(g.device, it->second), transp = true;
            else if (it->second.usable)
                shadow = GetShadow(g.device, it->second);
            else if (g_liveTessOn && it->second.tessUsable && g.phase != Phase::Capturing && state.depth == g_liveMainDepth && kind != 2)
                shadow = GetShadow(g.device, it->second), tess = true;
            else if (liveTransp && it->second.transparentUsable && state.depth == g_liveMainDepth && (!g_liveGbuffer[0] || state.gbufferSet != g_liveGbuffer[0]))
                shadow = GetShadow(g.device, it->second), transp = true;
        }
    }
    if (slot < 0)
    {
        g.offScreenDraws++;
        return;
    }
    if (!shadow)
    {
        g.skippedDraws++;
        if (g.phase != Phase::Capturing)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (state.depth == g_liveMainDepth)
                if (const auto pt = g_pipelines.find(state.pipeline); pt != g_pipelines.end())
                {
                    const uint64_t v = kind == 2 ? 0 : uint64_t(params[0]) * std::max(1u, params[1]);
                    if (pt->second.tessellated)
                        g_diagTessDraws++, g_diagTessVerts += v;
                    else if (pt->second.depthTested && !pt->second.writesDepth && pt->second.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
                    {
                        g_diagTranspDraws++, g_diagTranspVerts += v, g_diagTranspMax = std::max(g_diagTranspMax, v);
                        if (pt->second.blends)
                        {
                            g_diagBlendDraws++, g_diagBlendVerts += v;
                            g_diagBlendSizes[v]++;
                        }
                    }
                }
        }
        return;
    }
    if (g.phase != Phase::Capturing)
    {
        // live mode: the whole screen stream (acceleration structure); from the sun's shadow
        // map, only marked draws; on screen, marked those that have a counterpart in the map
        const bool screen = noDepth || g.screenLike[state.depth] == 1;
        bool sunMap = g_liveSunDepth && state.depth == g_liveSunDepth && !g_liveSunUnused;
        if (!screen && !sunMap)
            return;
        uint64_t key = kind * 1000003ull;
        for (uint32_t p : params)
            key = key * 1315423911ull + p;
        // sun shadows off (occlusion only): the sun map only serves to measure the sun's direction, which changes slowly;
        // an eighth of its draws every frame, in turn (it made 70 % of the replayed vertices: 1 million per present)
        if (sunMap && !screen && g_liveSunEighth && ((key * 0x9E3779B97F4A7C15ull) >> 61) != (g_liveWindow & 7))
            return;
        // size of the region: known vertices of the draw (triangle lists); indirect: fixed budget
        // rounded to three vertices: the acceleration structure forms triangles by triplets from the
        // start of the stream; a region of another multiple would shift all the following ones (giant triangles
        // linking different objects)
        uint64_t vertices = (kind == 2 ? 65536ull : uint64_t(params[0]) * std::max(1u, params[1]) + 2) / 3 * 3;
        // tessellated: the control points do not tell how many triangles come out; first estimate 32 vertices per point, then the learned size
        if (tess)
            vertices = std::min<uint64_t>(vertices * 32, 196608);
        LivePending pend;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const bool mainScreen = screen && state.depth == g_liveMainDepth;
            uint64_t idKey = kind * 1000003ull;
            uint32_t mesh = 0;
            bool probe = false;
            uint32_t part = 0;
            // transparent surface: only on the main screen, without identity or per-mesh budget, within the
            // layer's limit
            // on another view, they count for the choice of the main screen: otherwise, once a wrong depth was
            // chosen (a game's water, 15 draws, right after the menu), the real one, whose draws all come through here, never
            // took over again
            if (transp && screen && !noDepth && state.depth != g_liveMainDepth)
                g.slotDraws[slot]++;
            if (transp && (!screen || kind == 2 || g_liveTranspVerts + vertices > kLiveTranspVertices))
            {
                g_liveTranspRefused++;
                return;
            }
            if (transp)
                g_liveTranspVerts += vertices;
            if (screen && !transp)
            {
                // identity stable from one frame to the next: the mesh (index or vertex count, start in
                // its buffers), not the instance count nor the first instance (vegetation drawn by
                // instances changes them every frame: in RDR2, 120,000 identities for 800 objects on screen);
                // an ExecuteIndirect: its command signature, its pipeline, the bound index and vertex
                // buffers, its maximum command count; not its argument buffer, which rotates in RDR2 (one
                // identity per frame: 107,000 measurements in memory for 900 objects, never known, the RT scene
                // drawn at random among them every frame)
                if (kind == 2)
                    idKey = ((((idKey * 1315423911ull + params[3]) * 1315423911ull + state.pipeline) * 1315423911ull + state.indexBuffer) * 1315423911ull +
                             state.vertexBuffer) * 1315423911ull + params[0];
                else
                {
                    idKey = (idKey * 1315423911ull + params[0]) * 1315423911ull + params[2];
                    if (kind == 1)
                        idKey = idKey * 1315423911ull + params[3];
                }
                mesh = MeshId(idKey);
                // learned size of this mesh (one signature serves draws of every size: its
                // maximum imposed 65,536 vertices on all of them)
                if (kind == 2 || tess)
                    if (const auto jt = g_liveLearned.find(mesh); jt != g_liveLearned.end())
                        vertices = std::min<uint64_t>(tess ? 196608 : 65536, (uint64_t(jt->second) * 5 / 4 + 64 + 2) / 3 * 3);
                // every draw counts for choosing the main screen, even outside the budget (otherwise another
                // less busy depth would end up in front)
                g.slotDraws[slot]++;
            }
            if (mainScreen && !transp)
            {
                g_liveSeen[mesh] += uint32_t(vertices);
                g.lists[uint64_t(list)].mainDraws++;
                g_liveMainRecorded++;
                if (g_liveBudget && !g_liveAdmit.count(mesh))
                {
                    // outside the chosen meshes: an unknown one gets in while the budget allows; otherwise
                    // (and for a known mesh whose measurement must be redone) it is measured in the measurement area,
                    // within its per-frame limit; the rest is not captured at all
                    const bool known = g_liveKnown.count(mesh) != 0;
                    if (known || g_liveReserved + vertices > g_liveBudgetVertices)
                    {
                        if ((known && !g_liveProbeWant.count(mesh)) || g_liveProbeVerts + vertices > g_liveProbeBudget)
                        {
                            g_liveOutOfBudget++;
                            return;
                        }
                        // huge mesh (grass, foliage: thousands of draws): one draw in N each round, not
                        // always the same. Each part has its own identities: with only the rank among captured draws, this
                        // round's part overwrote the previous round's in the cache, and grass and foliage appeared there in
                        // pieces, constantly replaced
                        if (const auto st = g_liveProbeStride.find(mesh); st != g_liveProbeStride.end())
                        {
                            const auto ph = g_liveProbePhase.find(mesh);
                            const uint32_t phase = ph != g_liveProbePhase.end() ? ph->second : 0u;
                            if (g_liveProbeDraws[mesh]++ % st->second != phase)
                            {
                                g_liveOutOfBudget++;
                                return;
                            }
                            part = phase + 1;
                        }
                        probe = true;
                        g_liveProbeVerts += vertices;
                    }
                }
            }
            bool marked = false;
            if (transp)
                marked = false;
            else if (sunMap)
            {
                if (g_liveSunMarked >= kLiveMaxSunMarks)
                    return;
                g_liveSunMarked++;
                g_liveSunKeys.insert(key);
                marked = true;
            }
            else
                marked = !probe && (g_liveSunKeys.count(key) || g_livePrevSunKeys.count(key));
            // region in this writing window's part of the stream: the draws from its start, the measurement
            // area from its end
            const uint32_t area = uint32_t(g_liveWindow % kLiveAreas);
            const uint32_t capacity = uint32_t(std::min<uint64_t>(vertices * 16, kLiveAreaBytes / 2));
            const uint32_t size = (capacity + 15) & ~15u;
            uint32_t &cursor = g_liveAreaCursor[slot][area], &probeUsed = g_liveAreaProbe[slot][area];
            if (!capacity || uint64_t(cursor) + probeUsed + size > kLiveAreaBytes || g_liveAreaCounters[area] >= kLiveAreaCounters)
            {
                if (screen && !probe)
                    g_liveOverflow++;
                return; // part full: this draw is not captured
            }
            // geometry cache: a quarter of the part stays for this frame's draws (water comes late in the game's
            // frame: with the region full of big cached objects, it was not captured one time in two)
            if (probe && uint64_t(cursor) + probeUsed + size + kLiveAreaBytes / 4 > kLiveAreaBytes)
            {
                g_liveOutOfBudget++;
                return;
            }
            const uint32_t base = uint32_t(area * kLiveAreaBytes);
            if (probe)
                probeUsed += size, pend.src = base + uint32_t(kLiveAreaBytes) - probeUsed;
            else
                pend.src = base + cursor, cursor += size;
            pend.counter = kSlots + area * kLiveAreaCounters + g_liveAreaCounters[area]++;
            pend.rankKey = idKey, pend.key = key, pend.window = g_liveWindow;
            pend.slot = uint32_t(slot), pend.capacity = capacity, pend.vertices = uint32_t(vertices), pend.sig = params[3];
            pend.screen = screen, pend.mainScreen = mainScreen, pend.probe = probe, pend.sunMap = sunMap, pend.marked = marked, pend.indirect = kind == 2;
            pend.stableOrder = kind != 2 && params[1] <= 1;
            pend.transp = transp;
            pend.part = part;
            pend.camSerial = g_liveCamSerialNow();
            std::memcpy(pend.viewport, state.viewport, sizeof(pend.viewport));
            if (mainScreen && !probe && !transp)
                g_liveReserved += vertices;
            g_livePending[(uint64_t)list].push_back(pend);
        }
        D3D12_STREAM_OUTPUT_BUFFER_VIEW view = { g.so->GetGPUVirtualAddress() + slot * kSlotBytes + pend.src, pend.capacity,
                                                 g.counters->GetGPUVirtualAddress() + pend.counter * 8ull };
        list->SetPipelineState(shadow);
        list->SOSetTargets(0, 1, &view);
        issue(list);
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW none = {};
        list->SOSetTargets(0, 1, &none);
        list->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(state.pipeline));
        g_capReplayed.fetch_add(1, std::memory_order_relaxed), g_capReplayVerts.fetch_add(pend.vertices, std::memory_order_relaxed);
        if (pend.probe)
            g_capProbeReplayed.fetch_add(1, std::memory_order_relaxed), g_capProbeVerts.fetch_add(pend.vertices, std::memory_order_relaxed);
        else
            (pend.sunMap ? g_capSunVerts : pend.transp ? g_capTranspVerts : pend.mainScreen ? g_capMainVerts : g_capOtherVerts).fetch_add(pend.vertices, std::memory_order_relaxed);
        return;
    }
    const uint32_t index = g.drawCount++;
    if (index >= kMaxDraws)
        return;
    g.slotDraws[slot]++;
    DrawRecord& r = g.draws[index];
    r.slot = uint32_t(slot), r.frame = uint32_t(g.frame), r.kind = kind, r.pipeline = state.pipeline;
    std::memcpy(r.params, params, sizeof(params));

    // start of the draw in the stream: the counter's value before it
    Transition(list, g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(g.marks.Get(), index * 16ull, g.counters.Get(), slot * 8ull, 8);
    Transition(list, g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);

    D3D12_STREAM_OUTPUT_BUFFER_VIEW view = { g.so->GetGPUVirtualAddress() + slot * kSlotBytes, kSlotBytes, g.counters->GetGPUVirtualAddress() + slot * 8ull };
    list->SetPipelineState(shadow);
    list->SOSetTargets(0, 1, &view);
    issue(list);
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW none = {};
    list->SOSetTargets(0, 1, &none);
    // the next draw must read the size filled by this one: otherwise two draws in flight write
    // at the same place (observed: 9.7 % coverage without this barrier)
    Transition(list, g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(g.marks.Get(), index * 16ull + 8, g.counters.Get(), slot * 8ull, 8); // end of the draw
    Transition(list, g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
    list->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(state.pipeline));
}

void CountDevice(command_list* cmd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g.drawsPerDevice[cmd->get_device()->get_native()]++;
}

// snapshots of the screen image's content before its 1st, 2nd, 8th and 32nd draws since its binding: in RDR2, the
// first draw writes the scene image itself (snapshot taken before: the previous image, about one time
// in two); live_uiselect keeps the earliest that already holds the current image
void LiveUiSnapshot(command_list* cmd)
{
    ID3D12Resource *target = nullptr, *snap = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g.lists.find(cmd->get_native());
        if (it == g.lists.end() || !it->second.backBuffer)
            return;
        // before the first draw (content left there: in RDR2 a previous image, UI included) and
        // before the second (the first wrote the scene image, not the UI yet); only in a
        // direct command list: a copy is forbidden in the others (the UI's "bundles"),
        // taking some at the 8th and 32nd draws crashed RDR2 in its menu (v56)
        if (it->second.backBufferDraws++ > 1)
            return;
        if (ID3D12GraphicsCommandList* native = Native(cmd); !native || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return;
        if (g_liveSnapCount >= kLiveSnaps || !g_liveSnap[g_liveSnapCount])
            return;
        target = reinterpret_cast<ID3D12Resource*>(it->second.backBuffer);
        // screen image recreated (switch to HDR, Alt+Tab, size change): the snapshot is taken only if it
        // has the buffers' layout (a copy of another format or size makes the game's list
        // invalid: a game crashed when switching to HDR)
        const D3D12_RESOURCE_DESC d = target->GetDesc();
        if (d.Format != g_liveSnapFp.Footprint.Format || d.Width != g_liveSnapFp.Footprint.Width || d.Height != g_liveSnapFp.Footprint.Height)
            return;
        snap = g_liveSnap[g_liveSnapCount++];
        fp = g_liveSnapFp;
    }
    ID3D12GraphicsCommandList* list = Native(cmd);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = target;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; // as a target: the state required to draw
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = snap;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = target;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
}

// one more draw in this list (present window of its first draw)
void NoteDraw(uint64_t list)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CommandListState& st = g.lists[list];
    if (st.draws++ == 0)
        st.window = g_presentIndex.load(std::memory_order_relaxed);
    if (st.backBuffer)
        st.drewBackBuffer = st.backBuffer;
}

// list reset without having been executed: its captured draws will never be filled
void OnResetCommandList(command_list* cmd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const auto it = g.lists.find(cmd->get_native()); it != g.lists.end())
        it->second.draws = 0, it->second.window = ~0ull, it->second.drewBackBuffer = 0, it->second.mainDraws = 0;
    g_livePending.erase(cmd->get_native());
}

// captured draws of a list submitted to the game's queue: they belong to the current present (rank,
// identity, place in the RT scene), whatever window the list was written in (called under g_mutex)
// material markers of a list: counted as soon as it executes, even if its draws wait for the next computation (they are removed from the list)
void LiveNoteMaterial(std::vector<LivePending>& pending)
{
    for (auto it = pending.begin(); it != pending.end();)
    {
        if (it->hudHandle)
        {
            // screen-sized image finished: the first one after the material is the UI-free image
            if (g_liveHudArmed)
            {
                g_liveHudArmed = false;
                g_liveHudVotes[it->hudHandle]++, g_liveHudVoteCount++;
            }
            if (it->hudMark >= 0)
                g_liveHudDone = it->hudMark, g_liveHudDonePresent = g_presentIndex.load(std::memory_order_relaxed), g_liveHudSerial++;
            if (g_liveTraceOn)
                Log("SANSHUD %llx tampon %d", (unsigned long long)it->hudHandle, it->hudMark);
            if (it->hudMark >= 0)
            {
                // the marker stays in place: read in execution order with the depth ends (LiveCommit), even if the list waits for the next computation
                it->hudSerialMark = g_liveHudSerial, it->hudHandle = 0, it->hudMark = -1;
                ++it;
                continue;
            }
            it = pending.erase(it);
            continue;
        }
        if (it->materialMark < 0)
        {
            ++it;
            continue;
        }
        g_liveHudArmed = true;
        g_liveMaterialDone = it->materialMark;
        if (g_liveWindowClosed && g_liveWindowMaterial < 0)
            g_liveWindowMaterial = it->materialMark; // frame closed, its material comes after its depth
        else
            g_liveMaterialSinceClose++;
        if (g_liveTraceOn)
            Log("MATERIAL buffer %d window closed %d", it->materialMark, int(g_liveWindowClosed));
        it = pending.erase(it);
    }
}

void LiveCommit(const std::vector<LivePending>& pending, uint32_t listMain, uint32_t counted)
{
    for (size_t index = 0; index < pending.size(); index++)
    {
        const LivePending& p = pending[index];
        if (p.depthMark >= 0)
        {
            // end of writing of the depth. Several per frame are possible: only the one that saw at least 60 % of the previous frame's
            // draws counts (otherwise the depth is not finished)
            g_liveMainSinceMark += p.mainBefore - counted;
            counted = p.mainBefore;
            const bool complete = g_liveMainSinceMark >= 16 && uint64_t(g_liveMainSinceMark) * 10 >= uint64_t(g_liveMainLast) * 6;
            if (g_liveTraceOn)
                Log("MARK buffer %d draws %u (previous frame %u) state %x %s", p.depthMark, g_liveMainSinceMark, g_liveMainLast, p.depthAfter, complete ? "complete" : "refused");
            if (!complete)
            {
                g_liveMarksRefused++;
                continue;
            }
            g_liveMainLast = g_liveMainSinceMark, g_liveMainSinceMark = 0;
            g_liveSnapDone = p.depthMark, g_liveSnapSerial++;
            if (g_liveMaterialNext == 0)
                g_liveHudArmed = true; // no material copied: the UI-free image is the first one finished after the depth
            g_liveWindowClosed = true;
            // material already passed in this frame: it is its own; otherwise, the next one
            g_liveWindowMaterial = g_liveMaterialSinceClose ? g_liveMaterialDone : -1;
            g_liveMaterialSinceClose = 0;
            if (index + 1 < pending.size() || listMain > counted)
            {
                LiveDeferred later;
                later.pending.assign(pending.begin() + index + 1, pending.end());
                later.listMain = listMain, later.counted = counted;
                g_liveDeferred.push_back(std::move(later));
            }
            return;
        }
        if (p.hudSerialMark)
        {
            // copy of the UI-free image: the one of the game frame whose depth has just been completed
            g_livePairSnap = g_liveSnapSerial, g_livePairHud = p.hudSerialMark;
            continue;
        }
        // part of the stream already reused by more recent draws (list executed three windows after its
        // writing): ignored
        if (g_liveWindow - p.window >= kLiveAreaMaxAge)
        {
            g_liveStaleDraws++;
            continue;
        }
        if (p.mainScreen && p.camSerial > g_liveCommitCam)
            g_liveCommitCam = p.camSerial;
        if (p.transp)
        {
            const uint32_t room = (p.capacity + 47) / 48 * 48;
            if (uint64_t(g_liveTranspCursor) + room <= uint64_t(kLiveTranspVertices) * 16)
            {
                g_liveTranspRegions.push_back({ g_liveTranspCursor, room, p.counter, 0 });
                g_liveTranspSrc.push_back(uint32_t(p.slot * kSlotBytes) + p.src);
                g_liveTranspCursor += room;
            }
            continue;
        }
        uint32_t id = 0;
        if (p.screen)
        {
            // rank among the draws with the same signature, in execution order (the same from one frame to the next)
            const uint64_t h = (p.rankKey ^ (p.rankKey >> 29)) * 0x9E3779B97F4A7C15ull + g_liveKeyRank[p.rankKey]++ + (uint64_t(p.part) << 24);
            id = uint32_t(h ^ (h >> 32)) | 1u; // 0: no identity
            if (p.indirect)
                g_liveIdSig[id] = p.sig;
            if (p.mainScreen)
            {
                const uint32_t meshNow = MeshId(p.rankKey);
                g_liveIdMesh[id] = meshNow;
                if (p.stableOrder)
                    g_liveStableNow.insert(meshNow);
                // viewport, counted at execution (in writing order, two frames mixed there)
                g_liveViewCounts[(std::min(65535u, uint32_t(p.viewport[0] + p.viewport[2] + 0.5f)) << 16) |
                                 std::min(65535u, uint32_t(p.viewport[1] + p.viewport[3] + 0.5f))]++;
            }
        }
        if (p.marked && g_liveMarks.size() < kLiveMaxMarks)
            g_liveMarks.push_back({ p.key, p.slot, p.sunMap, { p.viewport[0], p.viewport[1], p.viewport[2], p.viewport[3] }, p.src, p.capacity, p.counter });
        if (p.probe)
        {
            g_liveProbes[p.slot].push_back({ p.src, p.capacity, p.counter, id });
            continue;
        }
        if (!p.screen)
            continue;
        uint32_t dst = 0, room = 0;
        if (p.mainScreen && g_livePersist)
        {
            // its place if it has one large enough (and not already taken in this frame), otherwise a new one:
            // the first free space that fits, otherwise at the end of the used part
            auto it = g_livePlaces.find(id);
            if (it != g_livePlaces.end() && it->second.lastFrame == g_livePlaceFrame)
                continue; // same identity twice in the frame: the second one is not kept
            if (it != g_livePlaces.end() && it->second.capacity < p.capacity)
                LiveFreePlace(it->second), g_livePlaces.erase(it), it = g_livePlaces.end();
            if (it == g_livePlaces.end())
            {
                // no margin: the structure is rebuilt every frame, a redistributed place costs nothing,
                // and every extra vertex (margin, hole) makes the rebuild heavier
                const uint32_t size = (p.capacity + 47) / 48 * 48;
                uint32_t offset = ~0u;
                for (auto f = g_liveFreeSpace.begin(); f != g_liveFreeSpace.end(); ++f)
                    if (f->second >= size)
                    {
                        offset = f->first;
                        const uint32_t rest = f->second - size, at = f->first + size;
                        g_liveFreeSpace.erase(f);
                        if (rest)
                            g_liveFreeSpace[at] = rest;
                        break;
                    }
                if (offset == ~0u)
                {
                    if (uint64_t(g_livePlaceHigh) + size > kSlotBytes)
                    {
                        g_liveOverflow++, g_livePlaceFailed++;
                        continue; // RT scene full: not kept (places are redistributed afterwards)
                    }
                    offset = g_livePlaceHigh, g_livePlaceHigh += size;
                }
                LivePlace place;
                place.offset = offset, place.capacity = size;
                if (!g_liveFreeCounts.empty())
                    place.countIndex = g_liveFreeCounts.back(), g_liveFreeCounts.pop_back();
                else
                    place.countIndex = g_liveNextCount++;
                it = g_livePlaces.emplace(id, place).first;
                g_livePlaceNew++;
            }
            it->second.prevFrame = it->second.lastFrame, it->second.lastFrame = g_livePlaceFrame, it->second.frameCounter = p.counter;
            it->second.stableOrder = p.stableOrder, it->second.rankKey = p.rankKey;
            dst = it->second.offset, room = it->second.capacity;
        }
        else
            dst = g_liveCursor[p.slot], room = (p.capacity + 47) / 48 * 48, g_liveCursor[p.slot] += room;
        g_liveRegions[p.slot].push_back({ dst, room, p.counter, id });
        g_liveRegionSrc[p.slot].push_back(p.src);
    }
    g_liveMainSinceMark += listMain - counted;
}

// list submitted to a queue: its draws count for the current present, not for the one they were written in
void OnExecuteCommandList(command_queue* queue, command_list* cmd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    {
        const auto st = g.lists.find(cmd->get_native());
        const uint32_t listMain = st != g.lists.end() ? st->second.mainDraws : 0;
        const auto pt = g_livePending.find(cmd->get_native());
        if (pt != g_livePending.end())
            LiveNoteMaterial(pt->second);
        if (g_liveWindowClosed)
        {
            // window closed: this list belongs to the next frame
            LiveDeferred later;
            if (pt != g_livePending.end())
                later.pending = std::move(pt->second);
            later.listMain = listMain;
            if (!later.pending.empty() || listMain)
                g_liveDeferred.push_back(std::move(later));
        }
        else if (pt != g_livePending.end())
            LiveCommit(pt->second, listMain, 0);
        else
            g_liveMainSinceMark += listMain;
        if (pt != g_livePending.end())
            g_livePending.erase(pt);
        if (st != g.lists.end())
            st->second.mainDraws = 0;
    }
    const auto it = g.lists.find(cmd->get_native());
    if (it == g.lists.end() || !it->second.draws)
        return;
    const bool late = it->second.window != g_presentIndex.load(std::memory_order_relaxed);
    (late ? g_execLate : g_execOnTime).fetch_add(it->second.draws, std::memory_order_relaxed);
    (late ? g_execLateLists : g_execLists).fetch_add(1, std::memory_order_relaxed);
    g_presentExecDraws.fetch_add(it->second.draws, std::memory_order_relaxed);
    g_diagExecQueues[queue ? queue->get_native() : 0] += it->second.draws;
    if (queue)
        g_liveExecQueues.insert(queue->get_native());
    if (it->second.drewBackBuffer)
        g_execBackBuffers.insert(it->second.drewBackBuffer);
    it->second.draws = 0, it->second.window = ~0ull, it->second.drewBackBuffer = 0;
}

bool OnDraw(command_list* cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    NoteDraw(cmd->get_native());
    if (g_liveCapture)
        LiveUiSnapshot(cmd);
    g_presentDraws.fetch_add(1, std::memory_order_relaxed);
    PassDraw(cmd->get_native(), vertexCount <= 4 && instanceCount == 1);
    if (g.phase == Phase::Capturing)
        CountDevice(cmd);
    if ((g.phase == Phase::Capturing || g_liveCapture) && OurDevice(cmd->get_device()))
        CaptureDraw(Native(cmd), 0, { vertexCount, instanceCount, firstVertex, firstInstance, 0 }, [&](ID3D12GraphicsCommandList* list) {
            list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
        });
    return false;
}

bool OnDrawIndexed(command_list* cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    NoteDraw(cmd->get_native());
    if (g_liveCapture)
        LiveUiSnapshot(cmd);
    g_presentDraws.fetch_add(1, std::memory_order_relaxed);
    PassDraw(cmd->get_native(), false);
    if (g.phase == Phase::Capturing)
        CountDevice(cmd);
    if ((g.phase == Phase::Capturing || g_liveCapture) && OurDevice(cmd->get_device()))
        CaptureDraw(Native(cmd), 1, { indexCount, instanceCount, firstIndex, uint32_t(vertexOffset), firstInstance }, [&](ID3D12GraphicsCommandList* list) {
            list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        });
    return false;
}

// ExecuteIndirect: ReShade's event does not give the command signature; the native call is replayed
// as it is with the shadow pipeline (a signature does not change pipelines), within a fixed vertex
// budget, the call's draws being appended to the stream
using PFN_ExecuteIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
PFN_ExecuteIndirect g_executeIndirect = nullptr;

void STDMETHODCALLTYPE HookExecuteIndirect(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* signature, UINT maxCount,
    ID3D12Resource* args, UINT64 argsOffset, ID3D12Resource* countBuffer, UINT64 countOffset)
{
    PassDraw(reinterpret_cast<uint64_t>(self), false);
    g_presentDraws.fetch_add(1, std::memory_order_relaxed);
    NoteDraw(reinterpret_cast<uint64_t>(self));
    if ((g.phase == Phase::Capturing || g_liveCapture) && g.device && self->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        ComPtr<ID3D12Device> owner;
        if (SUCCEEDED(self->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get() == g.device)
            CaptureDraw(self, 2, { maxCount, uint32_t(argsOffset), uint32_t(reinterpret_cast<uintptr_t>(args)), uint32_t(reinterpret_cast<uintptr_t>(signature)), 0 }, [&](ID3D12GraphicsCommandList* list) {
                g_executeIndirect(list, signature, maxCount, args, argsOffset, countBuffer, countOffset);
            });
    }
    g_executeIndirect(self, signature, maxCount, args, argsOffset, countBuffer, countOffset);
}

void OnInitCommandList(command_list* cmd)
{
    if (cmd->get_device()->get_api() != device_api::d3d12)
        return;
    ID3D12GraphicsCommandList* list = Native(cmd);
    if (list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
        PatchVtable(list, 59, reinterpret_cast<void*>(&HookExecuteIndirect), reinterpret_cast<void**>(&g_executeIndirect), "ExecuteIndirect");
}

// copy of the captured frame's depth, at its clear in the next frame: D3D12
// then guarantees the DEPTH_WRITE state
void CopyDepthAtClear(command_list* cmd, resource_view dsv)
{
    if (g.phase != Phase::WaitingDepth || g.depthCopied || !dsv.handle || !OurDevice(cmd->get_device()) || !g.depthReadback)
        return;
    const resource r = cmd->get_device()->get_resource_from_view(dsv);
    if (r.handle != g.depthResource)
        return;
    ID3D12GraphicsCommandList* list = Native(cmd);
    ID3D12Resource* depth = reinterpret_cast<ID3D12Resource*>(r.handle);
    Transition(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = g.depthReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g.depthFootprint;
    src.pResource = depth;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    g.depthCopied = true;
}

// Depth copy by ReShade (Generic Depth module, urti_depth.fx effect): the safest route
// in a real game, ReShade knowing when and how to copy it; its copy is readable by shaders.
void OnFinishEffects(effect_runtime* runtime, command_list* cmd, resource_view, resource_view)
{
    if (g.phase != Phase::WaitingDepth || g.depthCopied || !OurDevice(runtime->get_device()))
        return;
    const effect_texture_variable var = runtime->find_texture_variable("urti.fx", "UrtiDepth");
    if (!var.handle)
        return;
    resource_view srv = {}, srvSrgb = {};
    runtime->get_texture_binding(var, &srv, &srvSrgb);
    if (!srv.handle)
        return;
    ID3D12Resource* copy = reinterpret_cast<ID3D12Resource*>(runtime->get_device()->get_resource_from_view(srv).handle);
    if (!copy)
        return;
    g.depthDesc = copy->GetDesc();
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&g.depthDesc, 0, 1, 0, &g.depthFootprint, nullptr, nullptr, &total);
    g.depthReadback = MakeBuffer(g.device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g.depthReadback)
        return;
    ID3D12GraphicsCommandList* list = Native(cmd);
    const D3D12_RESOURCE_STATES srvState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(list, copy, srvState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = g.depthReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g.depthFootprint;
    src.pResource = copy;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(list, copy, D3D12_RESOURCE_STATE_COPY_SOURCE, srvState);
    g.depthCopied = true;
    Log("URTI: depth copied by ReShade (%llux%u, format %d)", (unsigned long long)g.depthDesc.Width, g.depthDesc.Height, (int)g.depthDesc.Format);
}

bool OnClearDepthStencil(command_list* cmd, resource_view dsv, const float*, const uint8_t*, uint32_t, const rect*)
{
    CopyDepthAtClear(cmd, dsv);
    return false;
}

bool OnBeginRenderPass(command_list* cmd, uint32_t count, const render_pass_render_target_desc* rts, const render_pass_depth_stencil_desc* ds, render_pass_flags)
{
    {
        resource_view views[8] = {};
        for (uint32_t i = 0; i < count && i < 8 && rts; i++)
            views[i] = rts[i].view;
        PassBind(cmd, std::min<uint32_t>(count, 8), views, ds ? ds->view : resource_view{ 0 });
    }
    // pass that clears its depth: called before BeginRenderPass, the depth is in DEPTH_WRITE
    if (ds && ds->depth_load_op == render_pass_load_op::clear)
        CopyDepthAtClear(cmd, ds->view);
    const uint64_t depth = ds && ds->view.handle ? cmd->get_device()->get_resource_from_view(ds->view).handle : 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.lists[cmd->get_native()].depth = depth;
    return false;
}

void Submit(ID3D12CommandQueue* queue)
{
    g.list->Close();
    ID3D12CommandList* lists[] = { g.list.Get() };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g.fence.Get(), ++g.fenceValue);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.fence->SetEventOnCompletion(g.fenceValue, e);
    WaitForSingleObject(e, 20000);
    CloseHandle(e);
}

void BeginList()
{
    g.allocator->Reset();
    g.list->Reset(g.allocator.Get(), nullptr);
}

// ------------------------------------------------------------------------------------------------
// image of the frame (to know what was captured)
// ------------------------------------------------------------------------------------------------

// copyOf: a buffer with the same layout (copy of the image, in state copyState) recorded instead of the image
void SaveBackBuffer(ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, const std::string& path, uint32_t step = 2, ID3D12Resource* copyOf = nullptr,
                    D3D12_RESOURCE_STATES copyState = D3D12_RESOURCE_STATE_COPY_DEST)
{
    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> readback = MakeBuffer(g.device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback)
        return;
    BeginList();
    Transition(g.list.Get(), backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = backBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (copyOf)
    {
        Transition(g.list.Get(), copyOf, copyState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.list->CopyBufferRegion(readback.Get(), 0, copyOf, 0, total);
        Transition(g.list.Get(), copyOf, D3D12_RESOURCE_STATE_COPY_SOURCE, copyState);
    }
    else
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(g.list.Get(), backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    Submit(queue);

    const uint8_t* data = nullptr;
    if (FAILED(readback->Map(0, nullptr, (void**)&data)))
        return;
    const uint32_t w = (uint32_t)desc.Width / step, h = desc.Height / step;
    // first image of a burst: raw texels next to it (same pitch), to compare encodings (the game's UI-free image)
    if (path.size() > 6 && path.compare(path.size() - 6, 6, "_0.ppm") == 0)
        if (FILE* raw = std::fopen((path.substr(0, path.size() - 4) + ".raw").c_str(), "wb"))
        {
            const uint32_t texel = uint32_t(fp.Footprint.RowPitch / std::max<uint64_t>(1, desc.Width)) >= 8 ? 8u : 4u;
            std::fprintf(raw, "URTIRAW %u %u %u %u\n", uint32_t(fp.Footprint.Format), w, h, texel);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    std::fwrite(data + fp.Offset + size_t(y * step) * fp.Footprint.RowPitch + size_t(x * step) * texel, 1, texel, raw);
            std::fclose(raw);
        }
    // HDR10: PQ -> nits (table), BT.2020 -> BT.709, reference white 203 nits, soft shoulder, gamma 2.2
    static float pq[1024] = {};
    if (pq[1023] == 0.0f)
        for (int i = 0; i < 1024; i++)
        {
            const double p = std::pow(i / 1023.0, 1.0 / 78.84375);
            pq[i] = float(10000.0 * std::pow(std::max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125) / 203.0);
        }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f)
    {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::vector<uint8_t> row(size_t(w) * 3);
        for (uint32_t y = 0; y < h; y++)
        {
            const uint8_t* src = data + fp.Offset + size_t(y * step) * fp.Footprint.RowPitch;
            for (uint32_t x = 0; x < w; x++)
            {
                uint8_t rgb[3] = {};
                if (fp.Footprint.Format == DXGI_FORMAT_R10G10B10A2_UNORM || fp.Footprint.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
                {
                    const uint32_t v = reinterpret_cast<const uint32_t*>(src)[x * step];
                    const float r = pq[v & 1023], gg = pq[(v >> 10) & 1023], b = pq[(v >> 20) & 1023];
                    const float lin[3] = { 1.6605f * r - 0.5876f * gg - 0.0728f * b, -0.1246f * r + 1.1329f * gg - 0.0083f * b,
                                           -0.0182f * r - 0.1006f * gg + 1.1187f * b };
                    for (int c = 0; c < 3; c++)
                    {
                        const float l = std::max(lin[c], 0.0f), t = l * (1.0f + l / 4.0f) / (1.0f + l);
                        rgb[c] = uint8_t(std::min(255.0f, 255.0f * std::pow(t, 1.0f / 2.2f)));
                    }
                }
                else if (fp.Footprint.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                {
                    const uint16_t* hf = reinterpret_cast<const uint16_t*>(src) + x * step * 4;
                    for (int c = 0; c < 3; c++)
                    {
                        const uint32_t hb = hf[c], e = (hb >> 10) & 31, m = hb & 1023;
                        const float v = e ? std::ldexp(1.0f + m / 1024.0f, int(e) - 15) : std::ldexp(m / 1024.0f, -14);
                        rgb[c] = uint8_t(std::min(255.0f, 255.0f * std::pow(std::max(v, 0.0f) / (1.0f + v), 1.0f / 2.2f)));
                    }
                }
                else
                {
                    const uint8_t* px = src + x * step * 4;
                    const bool bgr = fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM || fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                                     fp.Footprint.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
                    rgb[0] = px[bgr ? 2 : 0], rgb[1] = px[1], rgb[2] = px[bgr ? 0 : 2];
                }
                std::memcpy(&row[size_t(x) * 3], rgb, 3);
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
    readback->Unmap(0, nullptr);
}

// ------------------------------------------------------------------------------------------------
// analysis: coverage of the game's depth by the captured geometry
// ------------------------------------------------------------------------------------------------

void WritePfm(const std::string& path, uint32_t w, uint32_t h, const std::function<float(uint32_t, uint32_t)>& value)
{
    if (FILE* f = std::fopen(path.c_str(), "wb"))
    {
        std::fprintf(f, "Pf\n%u %u\n-1.0\n", w, h);
        for (int y = int(h) - 1; y >= 0; y--)
            for (uint32_t x = 0; x < w; x++)
            {
                const float v = value(x, uint32_t(y));
                std::fwrite(&v, 4, 1, f);
            }
        std::fclose(f);
    }
}

void Analyse(const std::vector<float>& depth, uint32_t width, uint32_t height, const uint8_t* so, const uint32_t* counters, uint32_t drawCount, const std::string& prefix)
{
    // reversed depth if more pixels at 0 than at 1 (sky); the clear value is then 0
    size_t zeros = 0, ones = 0;
    for (const float v : depth)
        zeros += v == 0.0f, ones += v == 1.0f;
    const bool reversed = zeros > ones;
    g.depthReversed = reversed;
    const float clearValue = reversed ? 0.0f : 1.0f;
    const uint32_t step = 2;
    const uint32_t w = width / step, h = height / step;
    const float empty = reversed ? 0.0f : 1.0f;
    std::vector<float> z(size_t(w) * h, empty);
    uint64_t triangles = 0, clipped = 0, drawsOnDepth = 0;

    if (g.captureSlot >= 0)
    {
        drawsOnDepth = g.slotDraws[g.captureSlot];
        const uint64_t filled = counters[g.captureSlot * 2];
        if (filled > kSlotBytes)
            Log("URTI: stream full (%llu MB requested): geometry truncated", (unsigned long long)(filled >> 20));
        const uint64_t bytes = std::min<uint64_t>(filled, kSlotBytes);
        const float* v = reinterpret_cast<const float*>(so + g.captureSlot * kSlotBytes);
        const uint64_t verts = bytes / 16;
        for (uint64_t t = 0; t + 2 < verts; t += 3)
        {
            const float* p[3] = { v + 4 * t, v + 4 * t + 4, v + 4 * t + 8 };
            if (p[0][3] <= 1e-5f || p[1][3] <= 1e-5f || p[2][3] <= 1e-5f)
            {
                clipped++; // triangles crossing the near plane: ignored in this test
                continue;
            }
            triangles++;
            float sx[3], sy[3], sz[3];
            for (int k = 0; k < 3; k++)
            {
                sx[k] = (p[k][0] / p[k][3] * 0.5f + 0.5f) * w;
                sy[k] = (0.5f - p[k][1] / p[k][3] * 0.5f) * h;
                sz[k] = p[k][2] / p[k][3];
            }
            const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (!(std::fabs(area) > 1e-12f))
                continue;
            const int x0 = std::max(0, (int)std::floor(std::min({ sx[0], sx[1], sx[2] })));
            const int x1 = std::min((int)w - 1, (int)std::ceil(std::max({ sx[0], sx[1], sx[2] })));
            const int y0 = std::max(0, (int)std::floor(std::min({ sy[0], sy[1], sy[2] })));
            const int y1 = std::min((int)h - 1, (int)std::ceil(std::max({ sy[0], sy[1], sy[2] })));
            if (x1 < x0 || y1 < y0 || (int64_t)(x1 - x0) * (y1 - y0) > (int64_t)w * h)
                continue;
            for (int y = y0; y <= y1; y++)
            {
                for (int x = x0; x <= x1; x++)
                {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float e0 = ((sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px - sx[1])) / area;
                    const float e1 = ((sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px - sx[2])) / area;
                    const float e2 = 1.0f - e0 - e1;
                    if (e0 < 0 || e1 < 0 || e2 < 0)
                        continue;
                    const float zz = e0 * sz[0] + e1 * sz[1] + e2 * sz[2];
                    if (zz < 0.0f || zz > 1.0f)
                        continue;
                    float& dst = z[size_t(y) * w + x];
                    dst = reversed ? std::max(dst, zz) : std::min(dst, zz);
                }
            }
        }
    }

    auto game = [&](uint32_t x, uint32_t y) { return depth[size_t(y * step + step / 2) * width + x * step + step / 2]; };
    WritePfm(prefix + "_game_depth.pfm", w, h, game);
    WritePfm(prefix + "_captured_depth.pfm", w, h, [&](uint32_t x, uint32_t y) { return z[size_t(y) * w + x]; });

    uint64_t valid = 0, covered = 0, extra = 0;
    double error = 0;
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            const float gz = game(x, y);
            const float ours = z[size_t(y) * w + x];
            const bool gameHas = gz != clearValue;
            const bool oursHas = ours != empty;
            if (gameHas)
            {
                valid++;
                const float diff = std::fabs(ours - gz);
                if (oursHas && diff <= 1e-4f + 1e-3f * std::fabs(gz))
                    covered++;
                if (oursHas)
                    error += diff;
            }
            else if (oursHas)
                extra++;
        }
    }
    Log("URTI: depth %ux%u %s; %llu draws on this depth, %llu triangles (%llu crossing the near plane)",
        width, height, reversed ? "reversed" : "normal", (unsigned long long)drawsOnDepth, (unsigned long long)triangles, (unsigned long long)clipped);
    Log("URTI: COVERAGE %.2f %% of the game's depth pixels (%llu / %llu); extra geometry on %llu pixels; mean deviation %.2e",
        valid ? 100.0 * covered / valid : 0.0, (unsigned long long)covered, (unsigned long long)valid, (unsigned long long)extra, valid ? error / valid : 0.0);
}

#include "urti_passes.inl"
#include "urti_live.inl"

// Full capture for offline analysis: header, draws (signature, bounds), stream of the
// main depth (vertices in clip space), the game's depth.
void DumpCapture(const std::string& path, const std::vector<float>& depth, uint32_t width, uint32_t height, const uint8_t* so, const uint32_t* counters)
{
    const uint64_t* marks = nullptr;
    if (g.captureSlot < 0 || FAILED(g.marksReadback->Map(0, nullptr, (void**)&marks)) || !marks)
        return;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
    {
        g.marksReadback->Unmap(0, nullptr);
        return;
    }
    const uint64_t filled = std::min<uint64_t>(counters[g.captureSlot * 2], kSlotBytes);
    const uint32_t header[12] = { 0x49545255u /* URTI */, 2, g.captureDrawCount, uint32_t(g.captureSlot), width, height, g.screenW, g.screenH,
                                  uint32_t(filled & 0xFFFFFFFF), uint32_t(filled >> 32), g.depthReversed ? 1u : 0u, kSlots };
    std::fwrite(header, sizeof(header), 1, f);
    // stream: depth (size, kind), bytes filled
    for (uint32_t s = 0; s < kSlots; s++)
    {
        uint32_t info[6] = {};
        if (g.slotDepth[s])
        {
            const D3D12_RESOURCE_DESC d = reinterpret_cast<ID3D12Resource*>(g.slotDepth[s])->GetDesc();
            const uint64_t bytes = std::min<uint64_t>(counters[s * 2], kSlotBytes);
            info[0] = uint32_t(d.Width), info[1] = d.Height, info[2] = uint32_t(g.screenLike[g.slotDepth[s]] + 1);
            info[3] = uint32_t(bytes & 0xFFFFFFFF), info[4] = uint32_t(bytes >> 32), info[5] = d.DepthOrArraySize;
        }
        std::fwrite(info, sizeof(info), 1, f);
    }
    for (uint32_t d = 0; d < g.captureDrawCount; d++)
    {
        const DrawRecord& r = g.draws[d];
        const uint32_t rec[10] = { r.slot, r.frame, r.kind, r.params[0], r.params[1], r.params[2], r.params[3], r.params[4],
                                   uint32_t(r.pipeline & 0xFFFFFFFF), uint32_t(r.pipeline >> 32) };
        std::fwrite(rec, sizeof(rec), 1, f);
        std::fwrite(&marks[d * 2], 16, 1, f); // start, end (bytes)
    }
    for (uint32_t s = 0; s < kSlots; s++)
        if (g.slotDepth[s])
            std::fwrite(so + s * kSlotBytes, 1, size_t(std::min<uint64_t>(counters[s * 2], kSlotBytes)), f);
    std::fwrite(depth.data(), sizeof(float), depth.size(), f);
    std::fclose(f);
    g.marksReadback->Unmap(0, nullptr);
    Log("URTI: capture written for analysis (%s, %.1f MB)", path.c_str(), (64.0 * g.captureDrawCount + filled + 4.0 * depth.size()) / 1048576.0);
}

void FinishCalibration()
{
    const uint8_t* soData = nullptr;
    const uint32_t* counterData = nullptr;
    const uint64_t* marks = nullptr;
    if (SUCCEEDED(g.soReadback->Map(0, nullptr, (void**)&soData)) && SUCCEEDED(g.counterReadback->Map(0, nullptr, (void**)&counterData)) &&
        SUCCEEDED(g.marksReadback->Map(0, nullptr, (void**)&marks)) && soData && counterData && marks)
        Calibrate(soData, counterData, marks);
    if (soData)
        g.soReadback->Unmap(0, nullptr);
    if (counterData)
        g.counterReadback->Unmap(0, nullptr);
    if (marks)
        g.marksReadback->Unmap(0, nullptr);
    g.liveCalibration = false;
}

void Finish(ID3D12CommandQueue* q)
{
    // the queue executed the next frame up to here: the depth copy is done
    BeginList();
    Submit(q);
    const std::string prefix = g_dir + "\\urti_capture" + std::to_string(g.captureIndex);
    const uint8_t* soData = nullptr;
    const uint32_t* counterData = nullptr;
    const uint8_t* depthData = nullptr;
    g.soReadback->Map(0, nullptr, (void**)&soData);
    g.counterReadback->Map(0, nullptr, (void**)&counterData);
    if (g.depthCopied && g.depthReadback && SUCCEEDED(g.depthReadback->Map(0, nullptr, (void**)&depthData)) && soData && counterData)
    {
        const uint32_t width = (uint32_t)g.depthDesc.Width, height = g.depthDesc.Height;
        const DXGI_FORMAT fmt = g.depthFootprint.Footprint.Format;
        const bool d32 = fmt == DXGI_FORMAT_R32_TYPELESS || fmt == DXGI_FORMAT_D32_FLOAT || fmt == DXGI_FORMAT_R32_FLOAT;
        std::vector<float> values(size_t(width) * height);
        for (uint32_t y = 0; y < height; y++)
        {
            const uint8_t* row = depthData + g.depthFootprint.Offset + size_t(y) * g.depthFootprint.Footprint.RowPitch;
            for (uint32_t x = 0; x < width; x++)
                values[size_t(y) * width + x] = d32 ? reinterpret_cast<const float*>(row)[x]
                                                    : (reinterpret_cast<const uint32_t*>(row)[x] & 0xFFFFFF) / 16777215.0f;
        }
        Analyse(values, width, height, soData, counterData, g.captureDrawCount, prefix);
        DumpCapture(prefix + ".urti", values, width, height, soData, counterData);
        g.depthReadback->Unmap(0, nullptr);
    }
    else
        Log("URTI: depth not copied (no clear seen in the next frame): no coverage test");
    if (soData)
        g.soReadback->Unmap(0, nullptr);
    if (counterData)
        g.counterReadback->Unmap(0, nullptr);
    g.depthReadback.Reset();
    g.captureIndex++;
}

void OnPresent(command_queue* queue, swapchain* swap, const rect*, const rect*, uint32_t, const rect*)
{
    device* dev = queue->get_device();
    if (dev->get_api() != device_api::d3d12)
        return;
    if (!g.device)
    {
        // the device that presents the image: the only one used
        if (!InitResources(reinterpret_cast<ID3D12Device*>(dev->get_native())))
        {
            Log("URTI: resources impossible to create, capture disabled");
            g.device = reinterpret_cast<ID3D12Device*>(1); // never retry
            return;
        }
        Log("URTI: presenting device chosen; %zu captures planned; F9: capture", g_captureFrames.size());
    }
    if (!OurDevice(dev) || !g.list)
        return;
    g_presentIndex.fetch_add(1, std::memory_order_relaxed);
    CamScanPresent();
    if (!g.screenW)
    {
        const resource back = swap->get_current_back_buffer();
        if (back.handle)
        {
            const D3D12_RESOURCE_DESC d = reinterpret_cast<ID3D12Resource*>(back.handle)->GetDesc();
            g.screenW = (uint32_t)d.Width, g.screenH = d.Height;
        }
    }
    ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());

    if (g_liveAtStart && g.frame == 0)
        live.enabled = true;
    if (g.frame == 0)
        LiveLoadCalibration(); // before any calibration decision
    // frames per second, logged every 300 presents (with or without RT effects)
    {
        static LARGE_INTEGER last = {}, freq = {};
        static double sum = 0;
        static uint32_t count = 0;
        static double gaps[300];
        static bool lastState = false;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        if (last.QuadPart && lastState == live.enabled)
            gaps[count] = double(now.QuadPart - last.QuadPart) / freq.QuadPart, sum += gaps[count], count++;
        else
            sum = 0, count = 0;
        last = now, lastState = live.enabled;
        if (count == 300)
        {
            Log("URTI: %.1f frames per second (RT effects %s)", count / sum, live.enabled ? "on" : "off");
            // time between presents: median and peaks (P95, P99), and the capture's cost over these 300 presents
            std::sort(gaps, gaps + count);
            const double tick = 1000.0 / double(freq.QuadPart);
            Log("URTI: presents in ms: median %.2f, P95 %.2f, P99 %.2f, worst %.2f; capture: %.2f ms of CPU per present, %.0f draws seen, "
                "%.0f replayed (%.0f k vertices), of which measurement area %.0f (%.0f k vertices)",
                gaps[count / 2] * 1000.0, gaps[count * 95 / 100] * 1000.0, gaps[count * 99 / 100] * 1000.0, gaps[count - 1] * 1000.0,
                double(g_capTicks.exchange(0)) * tick / count, double(g_capCalls.exchange(0)) / count, double(g_capReplayed.exchange(0)) / count,
                double(g_capReplayVerts.exchange(0)) / count / 1000.0, double(g_capProbeReplayed.exchange(0)) / count, double(g_capProbeVerts.exchange(0)) / count / 1000.0);
            Log("URTI: vertices replayed per present (k): main screen %.0f, sun map %.0f, transparents %.0f, other screens %.0f",
                double(g_capMainVerts.exchange(0)) / count / 1000.0, double(g_capSunVerts.exchange(0)) / count / 1000.0, double(g_capTranspVerts.exchange(0)) / count / 1000.0,
                double(g_capOtherVerts.exchange(0)) / count / 1000.0);
            sum = 0, count = 0;
        }
    }
    if (GetAsyncKeyState(VK_F7) & 1)
    {
        live.enabled = !live.enabled;
        Log("URTI: live RT effects %s", live.enabled ? "on" : "off");
    }
    g_presentDraws.exchange(0);
    // generated frame: no game draw executed since the previous present (written: some engines often write
    // the next frame's before presenting, and one frame in six looked generated)
    live.lastPresentDraws = g_presentExecDraws.exchange(0);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& pq = g_diagPresentQueues[queue->get_native()];
        pq.first++, pq.second += live.lastPresentDraws ? 1 : 0;
        g_diagPattern = (g_diagPattern << 1) | (live.lastPresentDraws ? 1 : 0);
        // generated frame (DLSS-G, FSR3): the game has not drawn into the presented image since the previous
        // present (the generated image is written by frame generation, outside the game's lists). When the game never draws
        // into the screen image (with DLSS-G some games draw into an intermediate image of
        // Streamline), the alternation of presents stands in: generation by interpolation presents
        // the generated image before the real one, so the one following the game's draws is the generated one, the next (without
        // an executed draw) the real one. Without alternation, a present without a draw simply reuses the previous image
        const uint64_t presented = swap ? swap->get_current_back_buffer().handle : 0;
        const bool drawn = presented && g_execBackBuffers.count(presented);
        g_execBackBuffers.clear();
        live.backBufferDrawn = (live.backBufferDrawn << 1) | (drawn ? 1u : 0u);
        live.presentNoDraw = (live.presentNoDraw << 1) | (live.lastPresentDraws ? 0u : 1u);
        const bool alternating = __popcnt(live.presentNoDraw & 0xFFFF) >= 6;
        live.imageGenerated = (live.backBufferDrawn & 0xFF) ? !drawn : alternating ? live.lastPresentDraws > 0 : live.lastPresentDraws == 0;
        g_diagBbPattern = (g_diagBbPattern << 1) | (drawn ? 1 : 0);
        if (g.frame % 300 == 7)
        {
            char text[512] = "";
            for (const auto& [q, c] : g_diagPresentQueues)
            {
                char item[64];
                std::snprintf(item, sizeof(item), " queue %04llx: %u (%u with draws)", (unsigned long long)(q & 0xFFFF), c.first, c.second);
                std::strncat(text, item, sizeof(text) - std::strlen(text) - 1);
            }
            char exec[256] = "";
            for (const auto& [q, d] : g_diagExecQueues)
            {
                char item[48];
                std::snprintf(item, sizeof(item), " file %04llx : %u", (unsigned long long)(q & 0xFFFF), d);
                std::strncat(exec, item, sizeof(exec) - std::strlen(exec) - 1);
            }
            char bits[65] = "", bbBits[65] = "";
            for (int k = 0; k < 64; k++)
                bits[k] = (g_diagPattern >> (63 - k)) & 1 ? '1' : '0', bbBits[k] = (g_diagBbPattern >> (63 - k)) & 1 ? '1' : '0';
            Log("URTI live: presents per queue:%s; draws executed per queue:%s; sequence (draws executed) %s; sequence (image drawn by "
                "the game) %s; draws ignored (executed too late) %u", text, exec, bits, bbBits, g_liveStaleDraws);
            Log("URTI live: main screen, ignored over 300 presents: %llu transparent draws (%llu vertices, the biggest %llu), "
                "%llu tessellated draws (%llu vertices)", (unsigned long long)g_diagTranspDraws, (unsigned long long)g_diagTranspVerts,
                (unsigned long long)g_diagTranspMax, (unsigned long long)g_diagTessDraws, (unsigned long long)g_diagTessVerts);
            char sizes[320] = "";
            {
                std::vector<std::pair<uint64_t, uint32_t>> v(g_diagBlendSizes.begin(), g_diagBlendSizes.end());
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (size_t i = 0; i < v.size() && i < 8; i++)
                {
                    char item[40];
                    std::snprintf(item, sizeof(item), " %llu×%u", (unsigned long long)v[i].first, v[i].second);
                    std::strncat(sizes, item, sizeof(sizes) - std::strlen(sizes) - 1);
                }
            }
            Log("URTI live: of which blended into the image (transparents): %llu draws, %llu vertices; biggest (vertices × draws):%s",
                (unsigned long long)g_diagBlendDraws, (unsigned long long)g_diagBlendVerts, sizes);
            g_diagTranspDraws = g_diagTranspVerts = g_diagTranspMax = g_diagTessDraws = g_diagTessVerts = 0;
            g_diagBlendDraws = g_diagBlendVerts = 0, g_diagBlendSizes.clear();
            g_diagPresentQueues.clear(), g_diagExecQueues.clear(), g_liveStaleDraws = 0;
        }
    }
    // G-buffer: the most drawn-into set of targets, reviewed every 30 frames (at once if there is none)
    if (g.frame % 30 == 0 || !g_liveGbufferCount)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const GbufferSet* best = nullptr;
        for (const auto& [key, set] : g_gbufferSets)
            if (set.draws && (!best || set.draws > best->draws))
                best = &set;
        if (best && (best->targets[0] != g_liveGbuffer[0] || best->count != g_liveGbufferCount))
        {
            char formats[64] = {};
            for (uint32_t k = 0, n = 0; k < best->count; k++)
                n += std::snprintf(formats + n, sizeof(formats) - n, k ? " %d" : "%d",
                                   best->targets[k] ? int(reinterpret_cast<ID3D12Resource*>(best->targets[k])->GetDesc().Format) : 0);
            Log("URTI live: G-buffer: %u targets (formats %s), %llu draws (%zu candidate sets of targets)", best->count, formats,
                (unsigned long long)best->draws, g_gbufferSets.size());
            std::memcpy(g_liveGbuffer, best->targets, sizeof(g_liveGbuffer));
            g_liveGbufferCount = best->count;
        }
        if (best || g.frame % 30 == 0)
            for (auto it = g_gbufferSets.begin(); it != g_gbufferSets.end();)
                it = it->second.draws ? (it->second.draws = 0, ++it) : g_gbufferSets.erase(it); // no longer drawn into: resources may be destroyed
    }
    if (GetAsyncKeyState(VK_F6) & 1)
        live.forceCalibration = true;
    if (GetAsyncKeyState(VK_F5) & 1)
        live.toggleSplit = true;
    // F4: full-screen before / after, instant (only laying the effect on the image is skipped, the computation goes on)
    if (GetAsyncKeyState(VK_F4) & 1)
    {
        live.showGameOnly = !live.showGameOnly;
        Log("URTI live: F4, %s", live.showGameOnly ? "the game's image alone (before)" : "image with URTI (after)");
    }
    if (const std::string f5 = g_dir + "\\urti_f5"; g.frame % 30 == 0 && GetFileAttributesA(f5.c_str()) != INVALID_FILE_ATTRIBUTES)
        DeleteFileA(f5.c_str()), live.toggleSplit = true; // F5 through a file (automated tests)
    // burst (F8 key, or trigger file urti_burst): 30 consecutive frames, each without and with URTI
    // (urti_burst_N\before_i.ppm, after_i.ppm), to measure in game what URTI adds as shimmer
    // (tools/analyse/burst_noise.py)
    const bool burstKey = (GetAsyncKeyState(VK_F8) & 1) != 0;
    if (const std::string burst = g_dir + "\\urti_burst"; live.burst == 0 &&
        (burstKey || (g.frame % 30 == 0 && GetFileAttributesA(burst.c_str()) != INVALID_FILE_ATTRIBUTES)))
    {
        DeleteFileA(burst.c_str());
        live.burstFolder = g_dir + "\\urti_burst_" + std::to_string(++live.burstCount);
        CreateDirectoryA(live.burstFolder.c_str(), nullptr);
        live.burst = 30, live.burstIndex = 0;
        Log("URTI live: burst %u requested (30 frames before / after URTI)", live.burstCount);
    }
    // real-pace film (trigger file urti_film): 30 consecutive presents, generated ones included, kept in video memory
    if (const std::string film = g_dir + "\\urti_film"; live.film == 0 && live.burst == 0 && g.frame % 30 == 0 && GetFileAttributesA(film.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA(film.c_str());
        live.filmFolder = g_dir + "\\urti_film_" + std::to_string(++live.filmCount);
        CreateDirectoryA(live.filmFolder.c_str(), nullptr);
        live.film = LiveState::kFilmFrames, live.filmIndex = 0;
        Log("URTI live: film %u requested (%u presents at real pace)", live.filmCount, LiveState::kFilmFrames);
    }
    g_liveCapture = live.enabled && g.phase == Phase::Idle;
    // full calibration (heavy: the whole stream read back) at start, on F6, or when frame-by-frame
    // sun tracking is lost; otherwise the sun is tracked on the GPU. Without RT sun shadows (the default
    // setting) and with the depth law known (also measured every frame), it is useless: in one game,
    // without a recognised shadow map, it restarted every 480 frames and the RT scene capture
    // stopped meanwhile (the effect disappeared and came back every five seconds)
    // Game without a read camera (Red Dead): the sun also serves the field-of-view measurement (its cascades): searched for as long as it is not tracked
    // (three attempts in a row at most, then at the lengthened interval)
    // (only in game: an RT scene of at least 20 draws computed less than 2 seconds ago. In the menu, a search every 120 frames
    // never found a screen, restarted endlessly and led Red Dead to "out of memory")
    const bool inGame = live.sceneStamp != ~0ull && live.frame < live.sceneStamp + 120 && live.sceneCount >= 20;
    const bool sunLost = g_camCbvSize == 0 && live.framesSinceSun >= 300 && inGame && !g_liveSunUnused;
    const bool calibrationUseful = live.sunShadows || !live.depthReady || sunLost;
    if (live.enabled && g.phase == Phase::Idle && g.nextCapture == ~0ull &&
        (live.forceCalibration || ((!live.calibrated || sunLost) && calibrationUseful && g.frame >= live.nextCalibration)))
    {
        g.liveCalibration = true;
        g.nextCapture = g.frame + 1;
        // (without a sun, night or overcast: more and more spaced out, up to 32 intervals; the scene capture stops for two presents)
        live.nextCalibration = g.frame + uint64_t(g_calibrationInterval) * (1ull << std::min<uint32_t>(live.missedCalibrations, sunLost ? 5u : 2u));
        live.forceCalibration = false;
    }
    const std::string photoTrigger = g_dir + "\\urti_f7";
    if (g.frame % 30 == 0 && GetFileAttributesA(photoTrigger.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA(photoTrigger.c_str()); // F7 toggle through a file (automated tests)
        live.enabled = !live.enabled;
        Log("URTI: live RT effects %s (file)", live.enabled ? "on" : "off");
    }

    if (g.phase == Phase::Idle)
        PassPresent(q);
    if ((GetAsyncKeyState(VK_F9) & 1) && g.phase == Phase::Idle)
    {
        g.nextCapture = g.frame + 1;
        Log("URTI: F9, capture of frame %llu", (unsigned long long)g.nextCapture);
    }
    // keyboard-free trigger (automated tests): file urti_capture_now next to the add-on
    if (g.frame % 30 == 0 && g.phase == Phase::Idle)
    {
        const std::string trigger = g_dir + "\\urti_capture_now";
        if (GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            DeleteFileA(trigger.c_str());
            g.nextCapture = g.frame + 1;
            Log("URTI: trigger, capture of frame %llu", (unsigned long long)g.nextCapture);
        }
    }
    while (g.captureListIndex < g_captureFrames.size() && g_captureFrames[g.captureListIndex] <= g.frame)
    {
        if (g.phase == Phase::Idle && g.nextCapture == ~0ull)
            g.nextCapture = g.frame + 1;
        g.captureListIndex++;
    }

    switch (g.phase.load())
    {
    case Phase::Idle:
        if (g.frame + 1 == g.nextCapture)
        {
            // stream counters reset before the captured frame
            BeginList();
            Transition(g.list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
            g.list->CopyBufferRegion(g.counters.Get(), 0, g.zeros.Get(), 0, kMaxDraws * 8ull);
            Transition(g.list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
            Submit(q);
            g.soUsed = 0;
            g.drawCount = 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (uint32_t s = 0; s < kSlots; s++)
                    g_liveRegions[s].clear(), g_liveRegionSrc[s].clear(), g_liveCursor[s] = 0;
                g_liveTranspRegions.clear(), g_liveTranspSrc.clear(), g_liveTranspCursor = 0;
            }
            g.frameDraws = 0;
            g.skippedDraws = 0;
            g.overflowDraws = 0;
            g.offScreenDraws = 0;
            for (uint32_t s = 0; s < kSlots; s++)
                g.slotDepth[s] = 0, g.slotDraws[s] = 0, g.slotScore[s] = 0.0f;
            g.presentsLeft = g.liveCalibration ? 2 : g_capturePresents;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g.drawsPerDevice.clear();
            }
            g.phase = Phase::Capturing;
        }
        break;

    case Phase::Capturing:
    {
        if (--g.presentsLeft > 0)
            break; // the capture goes on with the next present
        g.phase = Phase::Idle; // no draw replayed any more
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::string perDevice;
            for (auto& d : g.drawsPerDevice)
                perDevice += " " + std::to_string(d.second) + (d.first == reinterpret_cast<uint64_t>(g.device) ? " (the presenting one)" : " (other)");
            Log("URTI: draws per device during the capture:%s", perDevice.empty() ? " none" : perDevice.c_str());
        }
        g.nextCapture = ~0ull;
        g.captureDrawCount = std::min<uint32_t>(g.drawCount, kMaxDraws);
        const uint64_t used = kSoBytes; // the counters tell what is filled

        // main depth: the stream that received the most draws
        g.depthResource = 0;
        g.captureSlot = -1;
        uint32_t best = 0;
        for (uint32_t s = 0; s < kSlots; s++)
            if (g.slotDepth[s] && g.screenLike[g.slotDepth[s]] == 1 && g.slotDraws[s] > best)
                best = g.slotDraws[s], g.depthResource = g.slotDepth[s], g.captureSlot = int(s);

        Log("URTI: capture %u, frame %llu: %u draws, %u captured, %u ignored (unusable pipeline), %u on an off-screen depth, %.1f MB of vertices; "
            "%u pipelines with a vertex shader of which %u usable, %d root signatures modified",
            g.captureIndex, (unsigned long long)g.frame, (unsigned)g.frameDraws, g.captureDrawCount, (unsigned)g.skippedDraws, (unsigned)g.offScreenDraws,
            used / 1048576.0, (unsigned)g_pipelinesSeen, (unsigned)g_pipelinesUsable, g_rootSignaturesPatched.load());

        // vertices and counters to readable memory; image of the frame
        BeginList();
        Transition(g.list.Get(), g.so.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(g.list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (used)
            g.list->CopyBufferRegion(g.soReadback.Get(), 0, g.so.Get(), 0, used);
        g.list->CopyBufferRegion(g.counterReadback.Get(), 0, g.counters.Get(), 0, kMaxDraws * 8ull);
        Transition(g.list.Get(), g.marks.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.list->CopyBufferRegion(g.marksReadback.Get(), 0, g.marks.Get(), 0, kMaxDraws * 16ull);
        Transition(g.list.Get(), g.marks.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(g.list.Get(), g.so.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
        Transition(g.list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
        Submit(q);
        if (g.liveCalibration)
        {
            FinishCalibration();
            break;
        }
        const resource back = swap->get_current_back_buffer();
        if (back.handle)
            SaveBackBuffer(q, reinterpret_cast<ID3D12Resource*>(back.handle), g_dir + "\\urti_capture" + std::to_string(g.captureIndex) + "_image.ppm");

        // the depth will be copied at its clear in the next frame
        ID3D12Resource* depth = reinterpret_cast<ID3D12Resource*>(g.depthResource);
        g.depthCopied = false;
        if (depth)
        {
            g.depthDesc = depth->GetDesc();
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&g.depthDesc, 0, 1, 0, &g.depthFootprint, nullptr, nullptr, &total);
            g.depthReadback = MakeBuffer(g.device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            Log("URTI: depth %llux%u, format %d, %u draws on it", (unsigned long long)g.depthDesc.Width, g.depthDesc.Height, (int)g.depthDesc.Format, best);
            g.depthWaits = 0;
            g.phase = Phase::WaitingDepth;
        }
        else
        {
            Log("URTI: no draw captured on a depth");
            g.captureIndex++;
        }
        break;
    }

    case Phase::WaitingDepth:
        if (!g.depthCopied && g.depthWaits++ < 2)
            break; // ReShade's copy (effects) comes after our present event
        g.phase = Phase::Idle;
        Finish(q);
        break;
    }
    g.frame++;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        ReadSettings(module);
        CamScanRegister();
        reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
        reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
        reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
        reshade::register_event<reshade::addon_event::execute_command_list>(OnExecuteCommandList);
        reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
        reshade::register_event<reshade::addon_event::bind_index_buffer>(OnBindIndexBuffer);
        reshade::register_event<reshade::addon_event::bind_vertex_buffers>(OnBindVertexBuffers);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
        reshade::register_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
        reshade::register_event<reshade::addon_event::barrier>(OnBarrier);
        reshade::register_event<reshade::addon_event::bind_viewports>(OnBindViewports);
        reshade::register_event<reshade::addon_event::init_resource>(OnInitResource);
        reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(OnClearDepthStencil);
        reshade::register_event<reshade::addon_event::draw>(OnDraw);
        reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);
        reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnOpenOverlay);
        break;
    case DLL_PROCESS_DETACH:
        UnpatchVtables();
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}

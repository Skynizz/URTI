// URTI (Universal Ray Tracing Injector), add-on ReShade, version 0.2 : capture de la géométrie d'un jeu
// DirectX 12 sans toucher à son rendu, et test de couverture.
//
// Pour chaque pipeline graphique du jeu, un pipeline « ombre » : même vertex shader, même signature
// racine, sortie de flux (stream output) de SV_Position, sans rastérisation ni pixel shader. Pendant
// la frame capturée, chaque dessin (direct ou ExecuteIndirect) est rejoué une fois avec ce pipeline :
// on obtient les triangles tels que le jeu les a transformés (animation, instanciation, compression
// comprises), en espace de découpage. Test : ces triangles, rastérisés à nouveau, doivent reproduire
// la profondeur du jeu (aucune caméra nécessaire).
//
// D3D12 exige ALLOW_STREAM_OUTPUT dans la signature racine : on l'ajoute à celles du jeu à leur
// création (ID3D12Device::CreateRootSignature).
//
// Prudence dans un vrai jeu :
//  - un jeu crée souvent plusieurs appareils D3D12 (essais, upscaleurs) : seul celui qui présente
//    l'image est utilisé, choisi à la première présentation ;
//  - la profondeur est copiée à l'effacement suivant, où D3D12 garantit son état (écriture de
//    profondeur), plutôt qu'à la présentation où son état est inconnu.
//
// Réglages : urti.ini à côté de l'add-on (capture_frames=3000,6000 ; sinon touche F9), ou variable
// d'environnement URTI_CAPTURE_FRAME. Sorties à côté de l'add-on (ou dans URTI_OUT) : urti_log.txt,
// profondeurs du jeu et de la géométrie captée (PFM), image de la frame (PPM).

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
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if URTI_NRD
// NRD et NRI (NVIDIA) : en-tetes au niveau global, le code d'integration est dans urti_nrd.inl
#include <windows.h>
#include <dbghelp.h>
#include <NRD.h>
#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIWrapperD3D12.h>
// échecs internes de la couche d'intégration rendus visibles (muets en Release)
#define NRD_INTEGRATION_ASSERT(expr, msg) do { if (!(expr)) { std::fprintf(stderr, "NRD : %s\n", msg); std::fflush(stderr); } } while (0)
#include <NRDIntegration.hpp>
#endif

using Microsoft::WRL::ComPtr;
using namespace reshade::api;

extern "C" __declspec(dllexport) const char* NAME = "URTI";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Universal Ray Tracing Injector : capture de la géométrie (version 0.2).";

namespace
{
// ------------------------------------------------------------------------------------------------
// journal et réglages
// ------------------------------------------------------------------------------------------------

std::string g_dir, g_logPath;
std::vector<uint64_t> g_captureFrames;
std::vector<uint64_t> g_photoFrames; // urti.ini photo_frames= : photos à ces présentations (bac à sable, tests)
uint32_t g_capturePresents = 4; // la capture dure plusieurs présentations (génération d'images : une sur deux sans dessin du jeu)
bool g_liveAtStart = false;      // urti.ini live=1 : ombres RT en direct dès le lancement
uint32_t g_calibrationInterval = 180;
bool g_liveCapture = false;
std::atomic<uint32_t> g_presentDraws{ 0 }; // dessins du jeu depuis la dernière présentation (0 : image générée)
// G-buffer du jeu : cibles de rendu liées avec la profondeur de l'écran (au moins trois)
uint64_t g_liveGbuffer[8] = {};
uint32_t g_liveGbufferCount = 0;
void LiveGbufferBarrier(command_list* cmd, uint32_t count, const resource* resources, const resource_usage* oldStates, const resource_usage* newStates);
// mode direct : dessins marqués de l'image en cours. La carte d'ombres du soleil et l'écran voient les
// mêmes maillages : les paires de même signature donnent la direction du soleil à chaque image (GPU)
struct LiveMark
{
    uint64_t key = 0;
    uint32_t slot = 0;
    bool sunMap = false;
    float viewport[4] = {}; // dans la carte d'ombres : la cascade du dessin
    uint32_t offset = 0, capacity = 0, counter = 0; // zone du dessin dans le flux de son emplacement
};
// en direct, chaque dessin a sa zone du flux et son compteur : aucune barrière entre dessins (le
// compteur commun imposait une attente du GPU à chaque dessin, des milliers par image)
// id : identité de l'objet dessiné, la même d'une image à l'autre (signature du dessin et rang parmi les
// dessins de même signature : un moteur dessine souvent le même maillage à plusieurs endroits)
struct LiveRegion
{
    uint32_t offset = 0, capacity = 0, counter = 0, id = 0;
};
std::unordered_map<uint64_t, uint32_t> g_liveKeyRank; // dessins de l'écran de chaque signature dans cette image
// budget de géométrie (urti_live.inl le met à jour à chaque image) : un objet déjà mesuré et peu important
// (petit, lointain) n'entre pas dans la scène RT ; ceux à re-mesurer sont captés dans la zone de mesure, à
// la fin de l'emplacement, hors de la structure d'accélération
std::vector<LiveRegion> g_liveProbes[4];
uint32_t g_liveProbeUsed[4] = {};
std::unordered_set<uint32_t> g_liveKnown, g_liveAdmit, g_liveProbeWant;
std::unordered_map<uint32_t, uint32_t> g_liveSeen; // identités de l'écran dans cette image -> sommets
// ExecuteIndirect : nombre de sommets inconnu avant le dessin ; on réservait 65 536 sommets à chacun (place
// vide prise dans l'emplacement et dans la structure). Taille apprise par identité (mesurée), marge comprise.
std::unordered_map<uint32_t, uint32_t> g_liveLearned;
// dans RDR2 l'ordre des ExecuteIndirect change d'une image à l'autre (identité instable) : la taille est
// aussi apprise par signature de commande (le plus grand nombre de sommets écrits par ses dessins)
std::unordered_map<uint32_t, uint32_t> g_liveLearnedSig, g_liveIdSig; // signature -> sommets ; identité -> signature (cette image)
uint64_t g_liveReserved = 0; // sommets réservés dans l'écran principal (bilan)
uint64_t g_liveBudgetVertices = 0, g_liveProbeBudget = 0, g_liveProbeVerts = 0; // budget, zone de mesure (par image)
// photos de l'écran au premier dessin de chaque liaison de l'image d'écran (urti_live.inl choisit celle
// d'avant l'interface du jeu : les reflets n'y prennent pas les menus, la composition épargne l'interface)
constexpr uint32_t kLiveSnaps = 4;
std::unordered_set<uint64_t> g_liveBackBuffers;
ID3D12Resource* g_liveSnap[kLiveSnaps] = {};
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_liveSnapFp = {};
uint32_t g_liveSnapCount = 0;
bool g_liveBudget = false;
uint64_t g_liveMainDepth = 0; // profondeur de l'écran principal (celle dont le flux fait la scène RT) : seule soumise au budget
uint64_t g_liveSlotLimit = 0; // essai (urti.ini slot_limit=, sommets) : emplacement réduit, pour reproduire la saturation
uint32_t g_liveOverflow = 0, g_liveOutOfBudget = 0;
std::vector<LiveRegion> g_liveRegions[4]; // par emplacement (kSlots), dans l'ordre d'attribution
uint32_t g_liveCursor[4] = {};            // octets attribués dans chaque emplacement
uint32_t g_liveNextCounter = 4;           // compteurs par dessin après ceux des emplacements
bool g_liveCombine = true;       // urti.ini combine=0 : ombres RT seules, sans tenir compte de celles du jeu
uint32_t g_liveDebugGame = 0;    // urti.ini debug_game_shadow=1 : affiche l'ombre du jeu recalculée
float g_liveGameBias = 0.0015f;  // urti.ini game_bias= : tolérance du test de la carte d'ombres du jeu
float g_liveRayBias = 0.003f;    // urti.ini ray_bias= : départ du rayon décalé vers le soleil (part de la distance)
float g_liveSunRadius = 0.5f;    // urti.ini sun_radius= (degrés) : taille apparente du soleil, adoucit les bords
std::vector<LiveMark> g_liveMarks;
std::unordered_set<uint64_t> g_liveSunKeys, g_livePrevSunKeys;
uint64_t g_liveSunDepth = 0; // profondeur de la carte d'ombres du soleil (trouvée à l'étalonnage)
uint32_t g_liveSunMapDesc[3] = {}; // largeur, hauteur, format de cette carte (étalonnage stocké)

uint32_t g_liveSunMarked = 0;
constexpr uint32_t kLiveMaxSunMarks = 1024, kLiveMaxMarks = 4096;
bool g_liveShowMask = false;     // urti.ini show_mask=1 : l'effet montre les ombres RT seules      // le mode direct capte la profondeur de l'écran à chaque image

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
                // « a-b » : toutes les valeurs de a à b
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
// signatures racines : ajout de ALLOW_STREAM_OUTPUT
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

// entrées de tables de fonctions remplacées, remises à l'origine quand l'add-on est déchargé : ReShade
// décharge et recharge les add-ons à chaque appareil (RDR2 en crée plusieurs au démarrage pour tester la
// carte graphique) ; une table qui pointerait encore vers le code d'un add-on déchargé ferait planter le
// jeu au premier appel (ucrtbase, 0xc0000409, sans OptiScaler qui chargeait ReShade une seule fois)
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
    Log("URTI : %s intercepté", name);
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
// pipelines du jeu et pipelines « ombre »
// ------------------------------------------------------------------------------------------------

struct PipelineInfo
{
    std::vector<uint8_t> vs;
    std::vector<std::string> semantics;
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    ID3D12RootSignature* rootSignature = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    bool usable = false; // vertex shader seul (pas de géométrie / tessellation / mesh)
    bool writesDepth = true; // passes d'éclairage différé, décals, transparents : profondeur en lecture seule
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
        case pipeline_subobject_type::geometry_shader:
        case pipeline_subobject_type::hull_shader:
        case pipeline_subobject_type::domain_shader:
        case pipeline_subobject_type::mesh_shader:
        case pipeline_subobject_type::amplification_shader:
        case pipeline_subobject_type::compute_shader:
            if (s.data && static_cast<const shader_desc*>(s.data)->code_size)
                other = true;
            break;
        case pipeline_subobject_type::input_layout:
        {
            const input_element* e = static_cast<const input_element*>(s.data);
            for (uint32_t k = 0; k < s.count; k++)
            {
                info.semantics.push_back(e[k].semantic ? e[k].semantic : "");
                D3D12_INPUT_ELEMENT_DESC d = {};
                d.SemanticIndex = e[k].semantic_index;
                d.Format = static_cast<DXGI_FORMAT>(e[k].format); // les formats de ReShade suivent DXGI
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
                info.writesDepth = d->depth_enable && d->depth_write_mask;
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
                other = true;
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
    // seule la géométrie qui écrit la profondeur compte : une passe plein écran qui la lit ne décrit pas la scène
    info.usable = !other && info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && info.rootSignature && info.writesDepth;
    if (info.usable)
        g_pipelinesUsable++;
    std::lock_guard<std::mutex> lock(g_mutex);
    PipelineInfo& stored = g_pipelines[pipe.handle] = std::move(info);
    for (size_t k = 0; k < stored.layout.size(); k++)
        stored.layout[k].SemanticName = stored.semantics[k].c_str(); // après le déplacement
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
            Log("URTI : pipeline ombre refusé (vertex shader de %zu octets)", info.vs.size());
    }
    return info.shadow.Get();
}

// ------------------------------------------------------------------------------------------------
// capture
// ------------------------------------------------------------------------------------------------

// Un flux par profondeur (l'écran, les cartes d'ombres) : tous les
// dessins d'un flux partagent son compteur, et D3D12 range leurs sorties à la suite, à leur taille
// exacte, dans l'ordre d'exécution sur le GPU
constexpr uint64_t kSoBytes = 512ull << 20; // 32 M sommets
constexpr uint32_t kSlots = 4; // l'écran et jusqu'à trois cartes d'ombres (direction du soleil)
constexpr uint64_t kSlotBytes = kSoBytes / kSlots;
constexpr uint32_t kMaxDraws = 65536;

// un dessin capturé : signature (pour le retrouver d'une frame à l'autre) et place dans son flux
struct DrawRecord
{
    uint32_t slot = 0, frame = 0, kind = 0; // kind : 0 direct, 1 indexé, 2 indirect
    uint32_t params[5] = {};                 // paramètres du dessin (nombres, premiers indices…)
    uint64_t pipeline = 0;
};

struct CommandListState
{
    uint64_t pipeline = 0;
    uint64_t depth = 0;
    uint64_t backBuffer = 0; // image d'écran liée en cible (photo avant l'interface au premier dessin)
    bool snapped = false;
    float viewport[4] = {}; // x, y, largeur, hauteur (le premier)
};

enum class Phase
{
    Idle,
    Capturing,    // frame en cours de capture
    WaitingDepth, // frame suivante : copie de la profondeur à son effacement
};

struct Capture
{
    ID3D12Device* device = nullptr; // celui qui présente
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
    std::unordered_map<uint64_t, uint32_t> depthStates; // dernier état connu (barrières)

    uint64_t depthResource = 0;
    std::atomic<bool> depthCopied{ false };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint = {};
    D3D12_RESOURCE_DESC depthDesc = {};
    uint32_t captureDrawCount = 0, captureIndex = 0, presentsLeft = 0, depthWaits = 0;
    uint64_t slotDepth[kSlots] = {};
    std::atomic<uint32_t> slotDraws[kSlots];
    std::atomic<uint32_t> offScreenDraws{ 0 };
    uint32_t screenW = 0, screenH = 0;
    std::unordered_map<uint64_t, int> screenLike; // 1 : profondeur de la taille de l'écran, 0 : autre, -1 : inutilisable
    int captureSlot = -1;
    bool depthReversed = false;
    bool liveCalibration = false; // capture d'étalonnage du mode direct : rien n'est écrit sur le disque
    std::unordered_map<uint64_t, uint32_t> drawsPerDevice; // diagnostic : dessins de tous les appareils pendant la capture
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

void OnInitDevice(device* dev)
{
    if (dev->get_api() != device_api::d3d12)
        return;
    PatchVtable(reinterpret_cast<void*>(dev->get_native()), 16, reinterpret_cast<void*>(&HookCreateRootSignature),
                reinterpret_cast<void**>(&g_createRootSignature), "CreateRootSignature");
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

// relevé des passes (urti_passes.inl)
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
    st.snapped = false;
    if (targets[0])
        if (auto it = g.screenLike.find(depth); it != g.screenLike.end() && it->second == 1 && count >= g_liveGbufferCount)
        {
            std::memcpy(g_liveGbuffer, targets, sizeof(targets));
            g_liveGbufferCount = std::min<uint32_t>(count, 8);
        }
}


// état de départ des profondeurs (une carte d'ombres peut rester en écriture sans jamais de barrière)
void OnInitResource(device*, const resource_desc& desc, const subresource_data*, resource_usage initialState, resource res)
{
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

// flux de la profondeur (créé à la première rencontre) ; -1 : profondeur hors écran ou plus de place
int SlotFor(uint64_t depth)
{
    if (!depth)
        return -1;
    auto it = g.screenLike.find(depth);
    if (it == g.screenLike.end())
    {
        const D3D12_RESOURCE_DESC d = reinterpret_cast<ID3D12Resource*>(depth)->GetDesc();
        int kind = -1; // -1 inutilisable, 0 autre (cartes d'ombres…), 1 taille de l'écran
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
        // étalonnage stocké : la carte d'ombres du soleil est la profondeur de même description
        if (!g_liveSunDepth && g_liveSunMapDesc[0] && kind == 0 && uint32_t(d.Width) == g_liveSunMapDesc[0] && d.Height == g_liveSunMapDesc[1] &&
            uint32_t(d.Format) == g_liveSunMapDesc[2])
        {
            g_liveSunDepth = depth;
            Log("URTI direct : carte d'ombres du soleil reconnue (%ux%u format %u)", g_liveSunMapDesc[0], g_liveSunMapDesc[1], g_liveSunMapDesc[2]);
        }
    }
    if (it->second < 0)
        return -1;
    // en direct, les emplacements ne servent qu'à l'écran et à la carte d'ombres du soleil, qui a le
    // dernier, réservé (sinon plusieurs profondeurs de la taille de l'écran les prennent tous)
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
        // emplacements pleins : celui qui a le moins servi dans cette image est repris
        uint32_t least = 0;
        for (uint32_t s = 1; s + 1 < kSlots; s++)
            if (g.slotDraws[s] < g.slotDraws[least])
                least = s;
        g.slotDepth[least] = depth;
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

// rejoue le dessin avec le pipeline ombre ; le dessin du jeu a lieu ensuite, inchangé
void CaptureDraw(ID3D12GraphicsCommandList* list, uint32_t kind, const uint32_t (&params)[5], const std::function<void(ID3D12GraphicsCommandList*)>& issue)
{
    g.frameDraws++;
    CommandListState state;
    ID3D12PipelineState* shadow = nullptr;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        state = g.lists[(uint64_t)list];
        slot = SlotFor(state.depth);
        auto it = g_pipelines.find(state.pipeline);
        if (slot >= 0 && it != g_pipelines.end() && it->second.usable)
            shadow = GetShadow(g.device, it->second);
    }
    if (slot < 0)
    {
        g.offScreenDraws++;
        return;
    }
    if (!shadow)
    {
        g.skippedDraws++;
        return;
    }
    if (g.phase != Phase::Capturing)
    {
        // mode direct : tout le flux de l'écran (structure d'accélération) ; de la carte d'ombres du
        // soleil, seulement des dessins marqués ; à l'écran, marqués ceux qui ont un pendant dans la carte
        const bool screen = g.screenLike[state.depth] == 1;
        const bool sunMap = g_liveSunDepth && state.depth == g_liveSunDepth;
        if (!screen && !sunMap)
            return;
        uint64_t key = kind * 1000003ull;
        for (uint32_t p : params)
            key = key * 1315423911ull + p;
        // taille de la zone : sommets connus du dessin (listes de triangles) ; indirect : budget fixe
        // arrondie à trois sommets : la structure d'accélération forme les triangles par triplets depuis le
        // début du flux ; une zone d'un autre multiple décalerait toutes les suivantes (triangles géants
        // reliant des objets différents)
        uint64_t vertices = (kind == 2 ? 65536ull : uint64_t(params[0]) * std::max(1u, params[1]) + 2) / 3 * 3;
        uint32_t capacity = uint32_t(std::min<uint64_t>(vertices * 16, kSlotBytes));
        LiveRegion region;
        bool probe = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            // identité, avant tout refus : le rang parmi les dessins de même signature ne doit pas dépendre
            // des dessins refusés (sinon les identités glissent quand l'emplacement est plein)
            uint32_t id = 0;
            const bool mainScreen = screen && state.depth == g_liveMainDepth;
            if (screen)
            {
                // identité stable d'une image à l'autre : le maillage (nombre d'indices ou de sommets, début dans
                // ses tampons), pas le nombre d'instances ni la première instance (une végétation dessinée par
                // instances en change à chaque image : dans RDR2, 120 000 identités pour 800 objets à l'écran) ;
                // un ExecuteIndirect : sa signature de commande, son tampon d'arguments et la position dans ce
                // tampon (l'ordre des ExecuteIndirect change d'une image à l'autre dans RDR2 : pas le rang)
                uint64_t idKey = kind * 1000003ull;
                if (kind == 2)
                    idKey = ((idKey * 1315423911ull + params[3]) * 1315423911ull + params[2]) * 1315423911ull + params[1];
                else
                {
                    idKey = (idKey * 1315423911ull + params[0]) * 1315423911ull + params[2];
                    if (kind == 1)
                        idKey = idKey * 1315423911ull + params[3];
                }
                const uint64_t h = (idKey ^ (idKey >> 29)) * 0x9E3779B97F4A7C15ull + g_liveKeyRank[idKey]++;
                id = uint32_t(h ^ (h >> 32)) | 1u; // 0 : pas d'identité
                if (kind == 2)
                {
                    g_liveIdSig[id] = params[3];
                    // taille apprise de ce dessin (une même signature sert à des dessins de toutes tailles : son
                    // maximum imposait 65 536 sommets à tous)
                    uint64_t learned = ~0ull;
                    if (const auto jt = g_liveLearned.find(id); jt != g_liveLearned.end())
                        learned = jt->second;
                    if (learned != ~0ull)
                    {
                        vertices = std::min<uint64_t>(65536, (learned * 5 / 4 + 64 + 2) / 3 * 3);
                        capacity = uint32_t(vertices * 16);
                    }
                }
                // tous les dessins comptent pour choisir l'écran principal, même hors budget (sinon une autre
                // profondeur, moins chargée, finirait par passer devant)
                g.slotDraws[slot]++;
            }
            if (mainScreen)
            {
                g_liveSeen[id] = uint32_t(vertices);
                if (g_liveBudget && !g_liveAdmit.count(id))
                {
                    // hors des objets choisis : un inconnu entre tant que la place du budget le permet ; sinon
                    // (et pour un objet connu dont la mesure est à refaire) il est mesuré dans la zone de mesure,
                    // dans sa limite par image ; le reste n'est pas capté du tout
                    const bool known = g_liveKnown.count(id) != 0;
                    if (known || g_liveReserved + vertices > g_liveBudgetVertices)
                    {
                        if ((known && !g_liveProbeWant.count(id)) || g_liveProbeVerts + vertices > g_liveProbeBudget)
                        {
                            g_liveOutOfBudget++;
                            return;
                        }
                        probe = true;
                        g_liveProbeVerts += vertices;
                    }
                }
            }
            bool marked = false;
            if (sunMap)
            {
                if (g_liveSunMarked >= kLiveMaxSunMarks)
                    return;
                g_liveSunMarked++;
                g_liveSunKeys.insert(key);
                marked = g_liveMarks.size() < kLiveMaxMarks;
                if (!marked)
                    return;
            }
            else
                marked = !probe && g_liveMarks.size() < kLiveMaxMarks && (g_liveSunKeys.count(key) || g_livePrevSunKeys.count(key));
            const uint64_t limit = g_liveSlotLimit ? std::min<uint64_t>(kSlotBytes, g_liveSlotLimit * 16) : kSlotBytes;
            if (!capacity || uint64_t(g_liveCursor[slot]) + g_liveProbeUsed[slot] + capacity > limit || g_liveNextCounter >= kMaxDraws)
            {
                if (screen && !probe)
                    g_liveOverflow++;
                return; // emplacement plein : ce dessin n'est pas capté
            }
            if (probe)
            {
                // zone de mesure : depuis la fin de l'emplacement, vers le début
                g_liveProbeUsed[slot] += (capacity + 15) & ~15u;
                region = { uint32_t(limit - g_liveProbeUsed[slot]), capacity, g_liveNextCounter++, id };
                g_liveProbes[slot].push_back(region);
            }
            else
            {
                region = { g_liveCursor[slot], capacity, g_liveNextCounter++, id };
                g_liveCursor[slot] += (capacity + 15) & ~15u;
                if (screen)
                    g_liveRegions[slot].push_back(region);
                if (mainScreen)
                    g_liveReserved += vertices;
            }
            if (marked)
                g_liveMarks.push_back({ key, uint32_t(slot), sunMap, { state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3] },
                                        region.offset, region.capacity, region.counter });
        }
        D3D12_STREAM_OUTPUT_BUFFER_VIEW view = { g.so->GetGPUVirtualAddress() + slot * kSlotBytes + region.offset, region.capacity,
                                                 g.counters->GetGPUVirtualAddress() + region.counter * 8ull };
        list->SetPipelineState(shadow);
        list->SOSetTargets(0, 1, &view);
        issue(list);
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW none = {};
        list->SOSetTargets(0, 1, &none);
        list->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(state.pipeline));
        return;
    }
    const uint32_t index = g.drawCount++;
    if (index >= kMaxDraws)
        return;
    g.slotDraws[slot]++;
    DrawRecord& r = g.draws[index];
    r.slot = uint32_t(slot), r.frame = uint32_t(g.frame), r.kind = kind, r.pipeline = state.pipeline;
    std::memcpy(r.params, params, sizeof(params));

    // début du dessin dans le flux : la valeur du compteur avant lui
    Transition(list, g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(g.marks.Get(), index * 16ull, g.counters.Get(), slot * 8ull, 8);
    Transition(list, g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);

    D3D12_STREAM_OUTPUT_BUFFER_VIEW view = { g.so->GetGPUVirtualAddress() + slot * kSlotBytes, kSlotBytes, g.counters->GetGPUVirtualAddress() + slot * 8ull };
    list->SetPipelineState(shadow);
    list->SOSetTargets(0, 1, &view);
    issue(list);
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW none = {};
    list->SOSetTargets(0, 1, &none);
    // le dessin suivant doit lire la taille remplie par celui-ci : sinon deux dessins en vol écrivent
    // au même endroit (constaté : 9,7 % de couverture sans cette barrière)
    Transition(list, g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(g.marks.Get(), index * 16ull + 8, g.counters.Get(), slot * 8ull, 8); // fin du dessin
    Transition(list, g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
    list->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(state.pipeline));
}

void CountDevice(command_list* cmd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g.drawsPerDevice[cmd->get_device()->get_native()]++;
}

// premier dessin dans l'image d'écran depuis sa liaison : photo de son contenu (avant ce dessin)
void LiveUiSnapshot(command_list* cmd)
{
    ID3D12Resource *target = nullptr, *snap = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g.lists.find(cmd->get_native());
        if (it == g.lists.end() || !it->second.backBuffer || it->second.snapped)
            return;
        it->second.snapped = true;
        if (g_liveSnapCount >= kLiveSnaps || !g_liveSnap[g_liveSnapCount])
            return;
        target = reinterpret_cast<ID3D12Resource*>(it->second.backBuffer);
        snap = g_liveSnap[g_liveSnapCount++];
        fp = g_liveSnapFp;
    }
    ID3D12GraphicsCommandList* list = Native(cmd);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = target;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; // en cible : l'état exigé pour dessiner
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

bool OnDraw(command_list* cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
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

// ExecuteIndirect : l'événement de ReShade ne donne pas la signature de commande ; on rejoue l'appel
// natif tel quel avec le pipeline ombre (une signature ne change pas de pipeline), dans un budget
// fixe de sommets, les dessins de l'appel s'ajoutant à la suite dans le flux
using PFN_ExecuteIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
PFN_ExecuteIndirect g_executeIndirect = nullptr;

void STDMETHODCALLTYPE HookExecuteIndirect(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* signature, UINT maxCount,
    ID3D12Resource* args, UINT64 argsOffset, ID3D12Resource* countBuffer, UINT64 countOffset)
{
    PassDraw(reinterpret_cast<uint64_t>(self), false);
    g_presentDraws.fetch_add(1, std::memory_order_relaxed);
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

// copie de la profondeur de la frame capturée, à son effacement dans la frame suivante : D3D12
// garantit alors l'état DEPTH_WRITE
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

// Copie de la profondeur par ReShade (module Generic Depth, effet urti_depth.fx) : la voie la plus sûre
// dans un vrai jeu, ReShade sachant quand et comment la copier ; sa copie est en lecture par les shaders.
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
    Log("URTI : profondeur copiée par ReShade (%llux%u, format %d)", (unsigned long long)g.depthDesc.Width, g.depthDesc.Height, (int)g.depthDesc.Format);
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
    // passe qui efface sa profondeur : appelé avant BeginRenderPass, la profondeur est en DEPTH_WRITE
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
// image de la frame (pour savoir ce qui a été capturé)
// ------------------------------------------------------------------------------------------------

// copyOf : un tampon de même disposition (copie de l'image, état COPY_DEST) enregistré à la place de l'image
void SaveBackBuffer(ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, const std::string& path, uint32_t step = 2, ID3D12Resource* copyOf = nullptr)
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
        Transition(g.list.Get(), copyOf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.list->CopyBufferRegion(readback.Get(), 0, copyOf, 0, total);
        Transition(g.list.Get(), copyOf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    else
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(g.list.Get(), backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    Submit(queue);

    const uint8_t* data = nullptr;
    if (FAILED(readback->Map(0, nullptr, (void**)&data)))
        return;
    const uint32_t w = (uint32_t)desc.Width / step, h = desc.Height / step;
    // HDR10 : PQ -> nits (table), BT.2020 -> BT.709, blanc de référence 203 nits, épaule douce, gamma 2.2
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
// analyse : couverture de la profondeur du jeu par la géométrie capturée
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
    // profondeur inversée si plus de pixels à 0 qu'à 1 (ciel) ; la valeur d'effacement est alors 0
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
            Log("URTI : flux plein (%llu Mo demandés) : géométrie tronquée", (unsigned long long)(filled >> 20));
        const uint64_t bytes = std::min<uint64_t>(filled, kSlotBytes);
        const float* v = reinterpret_cast<const float*>(so + g.captureSlot * kSlotBytes);
        const uint64_t verts = bytes / 16;
        for (uint64_t t = 0; t + 2 < verts; t += 3)
        {
            const float* p[3] = { v + 4 * t, v + 4 * t + 4, v + 4 * t + 8 };
            if (p[0][3] <= 1e-5f || p[1][3] <= 1e-5f || p[2][3] <= 1e-5f)
            {
                clipped++; // triangles qui traversent le plan proche : ignorés dans ce test
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
    WritePfm(prefix + "_profondeur_jeu.pfm", w, h, game);
    WritePfm(prefix + "_profondeur_captee.pfm", w, h, [&](uint32_t x, uint32_t y) { return z[size_t(y) * w + x]; });

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
    Log("URTI : profondeur %ux%u %s ; %llu dessins sur cette profondeur, %llu triangles (%llu traversant le plan proche)",
        width, height, reversed ? "inversée" : "normale", (unsigned long long)drawsOnDepth, (unsigned long long)triangles, (unsigned long long)clipped);
    Log("URTI : COUVERTURE %.2f %% des pixels de profondeur du jeu (%llu / %llu) ; géométrie en trop sur %llu pixels ; écart moyen %.2e",
        valid ? 100.0 * covered / valid : 0.0, (unsigned long long)covered, (unsigned long long)valid, (unsigned long long)extra, valid ? error / valid : 0.0);
}

#include "urti_passes.inl"
#include "urti_live.inl"

// Capture complète pour l'analyse hors ligne : en-tête, dessins (signature, bornes), flux de la
// profondeur principale (sommets en espace de découpage), profondeur du jeu.
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
    // flux : profondeur (taille, sorte), octets remplis
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
        std::fwrite(&marks[d * 2], 16, 1, f); // début, fin (octets)
    }
    for (uint32_t s = 0; s < kSlots; s++)
        if (g.slotDepth[s])
            std::fwrite(so + s * kSlotBytes, 1, size_t(std::min<uint64_t>(counters[s * 2], kSlotBytes)), f);
    std::fwrite(depth.data(), sizeof(float), depth.size(), f);
    std::fclose(f);
    g.marksReadback->Unmap(0, nullptr);
    Log("URTI : capture écrite pour l'analyse (%s, %.1f Mo)", path.c_str(), (64.0 * g.captureDrawCount + filled + 4.0 * depth.size()) / 1048576.0);
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
    // la file a exécuté la frame suivante jusqu'ici : la copie de profondeur est faite
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
        Log("URTI : profondeur non copiée (aucun effacement vu dans la frame suivante) : pas de test de couverture");
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
        // l'appareil qui présente l'image : c'est le seul qu'on utilise
        if (!InitResources(reinterpret_cast<ID3D12Device*>(dev->get_native())))
        {
            Log("URTI : ressources impossibles à créer, capture désactivée");
            g.device = reinterpret_cast<ID3D12Device*>(1); // ne plus réessayer
            return;
        }
        Log("URTI : appareil qui présente choisi ; %zu captures prévues ; F9 : capture", g_captureFrames.size());
    }
    if (!OurDevice(dev) || !g.list)
        return;
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
        LiveLoadCalibration(); // avant toute décision d'étalonnage
    // images par seconde, journalisées toutes les 300 présentations (avec ou sans ombres RT)
    {
        static LARGE_INTEGER last = {}, freq = {};
        static double sum = 0;
        static uint32_t count = 0;
        static bool lastState = false;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        if (last.QuadPart && lastState == live.enabled)
            sum += double(now.QuadPart - last.QuadPart) / freq.QuadPart, count++;
        else
            sum = 0, count = 0;
        last = now, lastState = live.enabled;
        if (count == 300)
        {
            Log("URTI : %.1f images par seconde (ombres RT %s)", count / sum, live.enabled ? "actives" : "coupées");
            sum = 0, count = 0;
        }
    }
    if (GetAsyncKeyState(VK_F7) & 1)
    {
        live.enabled = !live.enabled;
        Log("URTI : ombres RT en direct %s", live.enabled ? "actives" : "coupées");
    }
    live.lastPresentDraws = g_presentDraws.exchange(0);
    if (GetAsyncKeyState(VK_F6) & 1)
        live.forceCalibration = true;
    if (GetAsyncKeyState(VK_F5) & 1)
        live.toggleSplit = true;
    if (const std::string f5 = g_dir + "\\urti_f5"; g.frame % 30 == 0 && GetFileAttributesA(f5.c_str()) != INVALID_FILE_ATTRIBUTES)
        DeleteFileA(f5.c_str()), live.toggleSplit = true; // F5 par fichier (tests automatiques)
    // rafale (touche F8, ou fichier déclencheur urti_rafale) : 30 images consécutives, chacune sans et avec URTI
    // (urti_rafale_N\avant_i.ppm, apres_i.ppm), pour mesurer dans le jeu ce qu'URTI ajoute comme tremblement
    // (tools/reshade/urti_rafale.py)
    const bool burstKey = (GetAsyncKeyState(VK_F8) & 1) != 0;
    if (const std::string burst = g_dir + "\\urti_rafale"; live.burst == 0 &&
        (burstKey || (g.frame % 30 == 0 && GetFileAttributesA(burst.c_str()) != INVALID_FILE_ATTRIBUTES)))
    {
        DeleteFileA(burst.c_str());
        live.burstFolder = g_dir + "\\urti_rafale_" + std::to_string(++live.burstCount);
        CreateDirectoryA(live.burstFolder.c_str(), nullptr);
        live.burst = 30, live.burstIndex = 0;
        Log("URTI direct : rafale %u demandée (30 images avant / après URTI)", live.burstCount);
    }
    g_liveCapture = live.enabled && g.phase == Phase::Idle;
    // étalonnage complet (lourd : tout le flux relu) au départ, sur F6, ou quand le suivi du soleil
    // image par image est perdu ; sinon le soleil est suivi sur le GPU
    if (live.enabled && g.phase == Phase::Idle && g.nextCapture == ~0ull &&
        (live.forceCalibration || (!live.calibrated && g.frame >= live.nextCalibration)))
    {
        g.liveCalibration = true;
        g.nextCapture = g.frame + 1;
        live.nextCalibration = g.frame + uint64_t(g_calibrationInterval) * (1ull << std::min<uint32_t>(live.missedCalibrations, 2));
        live.forceCalibration = false;
    }
    const std::string photoTrigger = g_dir + "\\urti_f7";
    if (g.frame % 30 == 0 && GetFileAttributesA(photoTrigger.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA(photoTrigger.c_str()); // bascule F7 par fichier (tests automatiques)
        live.enabled = !live.enabled;
        Log("URTI : ombres RT en direct %s (fichier)", live.enabled ? "actives" : "coupées");
    }

    if (g.phase == Phase::Idle)
        PassPresent(q);
    if ((GetAsyncKeyState(VK_F9) & 1) && g.phase == Phase::Idle)
    {
        g.nextCapture = g.frame + 1;
        Log("URTI : F9, capture de la frame %llu", (unsigned long long)g.nextCapture);
    }
    // déclencheur sans clavier (tests automatiques) : fichier urti_capture_maintenant à côté de l'add-on
    if (g.frame % 30 == 0 && g.phase == Phase::Idle)
    {
        const std::string trigger = g_dir + "\\urti_capture_maintenant";
        if (GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            DeleteFileA(trigger.c_str());
            g.nextCapture = g.frame + 1;
            Log("URTI : déclencheur, capture de la frame %llu", (unsigned long long)g.nextCapture);
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
            // compteurs du flux remis à zéro avant la frame capturée
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
                    g_liveRegions[s].clear(), g_liveCursor[s] = 0;
                g_liveNextCounter = kSlots;
            }
            g.frameDraws = 0;
            g.skippedDraws = 0;
            g.overflowDraws = 0;
            g.offScreenDraws = 0;
            for (uint32_t s = 0; s < kSlots; s++)
                g.slotDepth[s] = 0, g.slotDraws[s] = 0;
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
            break; // la capture continue sur la présentation suivante
        g.phase = Phase::Idle; // plus aucun dessin rejoué
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::string perDevice;
            for (auto& d : g.drawsPerDevice)
                perDevice += " " + std::to_string(d.second) + (d.first == reinterpret_cast<uint64_t>(g.device) ? " (celui qui présente)" : " (autre)");
            Log("URTI : dessins par appareil pendant la capture :%s", perDevice.empty() ? " aucun" : perDevice.c_str());
        }
        g.nextCapture = ~0ull;
        g.captureDrawCount = std::min<uint32_t>(g.drawCount, kMaxDraws);
        const uint64_t used = kSoBytes; // les compteurs disent ce qui est rempli

        // profondeur principale : le flux qui a reçu le plus de dessins
        g.depthResource = 0;
        g.captureSlot = -1;
        uint32_t best = 0;
        for (uint32_t s = 0; s < kSlots; s++)
            if (g.slotDepth[s] && g.screenLike[g.slotDepth[s]] == 1 && g.slotDraws[s] > best)
                best = g.slotDraws[s], g.depthResource = g.slotDepth[s], g.captureSlot = int(s);

        Log("URTI : capture %u, frame %llu : %u dessins, %u capturés, %u ignorés (pipeline inutilisable), %u sur une profondeur hors écran, %.1f Mo de sommets ; "
            "%u pipelines à vertex shader dont %u utilisables, %d signatures racines modifiées",
            g.captureIndex, (unsigned long long)g.frame, (unsigned)g.frameDraws, g.captureDrawCount, (unsigned)g.skippedDraws, (unsigned)g.offScreenDraws,
            used / 1048576.0, (unsigned)g_pipelinesSeen, (unsigned)g_pipelinesUsable, g_rootSignaturesPatched.load());

        // sommets et compteurs vers la mémoire lisible ; image de la frame
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

        // la profondeur sera copiée à son effacement dans la frame suivante
        ID3D12Resource* depth = reinterpret_cast<ID3D12Resource*>(g.depthResource);
        g.depthCopied = false;
        if (depth)
        {
            g.depthDesc = depth->GetDesc();
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&g.depthDesc, 0, 1, 0, &g.depthFootprint, nullptr, nullptr, &total);
            g.depthReadback = MakeBuffer(g.device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            Log("URTI : profondeur %llux%u, format %d, %u dessins dessus", (unsigned long long)g.depthDesc.Width, g.depthDesc.Height, (int)g.depthDesc.Format, best);
            g.depthWaits = 0;
            g.phase = Phase::WaitingDepth;
        }
        else
        {
            Log("URTI : aucun dessin capturé sur une profondeur");
            g.captureIndex++;
        }
        break;
    }

    case Phase::WaitingDepth:
        if (!g.depthCopied && g.depthWaits++ < 2)
            break; // la copie de ReShade (effets) vient après notre événement de présentation
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
        reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
        reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
        reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
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

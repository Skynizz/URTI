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
// dessins exécutés par la file du jeu : écrits dans la même fenêtre de présentation, ou dans une précédente
std::atomic<uint64_t> g_presentIndex{ 0 };
// coût de la capture (journal) : temps processeur dans CaptureDraw (unités QueryPerformanceCounter), dessins vus, dessins rejoués et leurs sommets
// (dont la zone de mesure)
std::atomic<uint64_t> g_capTicks{ 0 }, g_capCalls{ 0 }, g_capReplayed{ 0 }, g_capReplayVerts{ 0 }, g_capProbeReplayed{ 0 }, g_capProbeVerts{ 0 };
std::atomic<uint64_t> g_capSunVerts{ 0 }, g_capTranspVerts{ 0 }, g_capMainVerts{ 0 }, g_capOtherVerts{ 0 }; // sommets rejoués : carte du soleil, surfaces transparentes, écran principal (hors mesure), autres écrans
std::atomic<uint32_t> g_execOnTime{ 0 }, g_execLate{ 0 }, g_execLists{ 0 }, g_execLateLists{ 0 };
std::atomic<uint32_t> g_presentExecDraws{ 0 }; // dessins exécutés depuis la dernière présentation (0 : image générée)
// diagnostic de la génération d'images : par file, présentations et présentations précédées de dessins exécutés ;
// files où le jeu exécute ses dessins ; suite des présentations (1 : dessins exécutés avant elle)
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_diagPresentQueues;
std::unordered_map<uint64_t, uint32_t> g_diagExecQueues;
uint64_t g_diagPattern = 0, g_diagBbPattern = 0;
// dessins de l'écran principal ignorés par la capture : surfaces transparentes (profondeur testée, pas écrite),
// tessellation
uint64_t g_diagTranspDraws = 0, g_diagTranspVerts = 0, g_diagTranspMax = 0, g_diagTessDraws = 0, g_diagTessVerts = 0;
uint64_t g_diagBlendDraws = 0, g_diagBlendVerts = 0;
std::map<uint64_t, uint32_t> g_diagBlendSizes; // sommets d'un dessin mélangé -> nombre de dessins
// images d'écran où le jeu a dessiné dans des listes exécutées depuis la dernière présentation
std::unordered_set<uint64_t> g_execBackBuffers;
// G-buffer du jeu : cibles de rendu liées avec la profondeur de l'écran (au moins trois). Parmi ces jeux de
// cibles, celui où le jeu dessine le plus : dans RDR2, une passe plus tardive (transparents) lie aussi cinq
// cibles, dont deux du G-buffer ; la dernière liaison vue la prenait une partie du temps (lissé perdu)
uint64_t g_liveGbuffer[8] = {};
uint32_t g_liveGbufferCount = 0;
struct GbufferSet
{
    uint64_t targets[8] = {};
    uint32_t count = 0;
    uint64_t draws = 0;
};
std::unordered_map<uint64_t, GbufferSet> g_gbufferSets; // clé : la première cible
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
std::unordered_map<uint64_t, uint32_t> g_liveKeyRank; // dessins de l'écran de chaque signature exécutés dans cette image
// Dessins rangés dans l'ordre où la file du jeu les exécute, pas dans celui où ils sont écrits : Horizon Forbidden
// West (Decima) écrit un tiers de ses listes de commandes avant la présentation précédente (image suivante préparée
// en avance) ; rangés à l'écriture, ses objets changeaient d'identité à chaque image (rang parmi les dessins de même
// signature mêlant deux images), leurs zones étaient lues avant d'être remplies et leurs compteurs repris par
// d'autres. À l'écriture, un dessin capté reçoit une zone dans la partie du flux de sa fenêtre d'écriture (trois
// parties tournantes) et un compteur de cette partie ; à l'exécution de sa liste, son rang, son identité, sa place
// dans la scène RT et la présentation à laquelle il appartient
struct LivePending
{
    uint64_t rankKey = 0, key = 0, window = 0;
    uint32_t slot = 0, src = 0, capacity = 0, counter = 0, vertices = 0, sig = 0;
    uint32_t part = 0; // maillage énorme capté par parts : numéro de la part + 1 (son identité ne prend pas la place des autres parts)
    // repère de fin d'écriture de la profondeur principale (pas un dessin) : tampon de sa copie, dessins de la liste avant lui
    int32_t depthMark = -1;
    uint32_t mainBefore = 0, depthAfter = 0;
    int32_t materialMark = -1; // repère de fin d'écriture de l'image de matériau : tampon de sa copie
    // fin d'écriture d'une image de la taille de l'écran en RGBA 16 bits flottants (candidate à l'image sans interface) : la ressource,
    // et le tampon de sa copie si c'est l'image adoptée
    uint64_t hudHandle = 0;
    int32_t hudMark = -1;
    uint64_t hudSerialMark = 0; // copie de l'image sans interface exécutée (son numéro) : repère laissé dans l'ordre des dessins, pour savoir de quelle image du jeu elle est
    bool screen = false, mainScreen = false, probe = false, sunMap = false, marked = false, indirect = false;
    // sommets dans le même ordre d'une image à l'autre : dessin direct d'un seul exemplaire (un ExecuteIndirect ou
    // des exemplaires choisis sur le GPU changent d'ordre et de nombre : leurs paires de sommets ne disent rien du
    // mouvement de la caméra)
    bool stableOrder = false;
    bool transp = false; // surface transparente (couche 2)
    float viewport[4] = {};
    uint32_t camSerial = 0; // caméra du jeu en cours quand ce dessin a été écrit (urti_camscan.inl)
};
uint32_t g_liveCommitCam = 0; // caméra du jeu du dernier dessin de l'écran principal soumis
constexpr uint32_t kLiveAreas = 4;
constexpr uint32_t kLiveAreaMaxAge = 3; // liste exécutée à partir de tant de fenêtres après son écriture : ignorée
std::unordered_map<uint64_t, std::vector<LivePending>> g_livePending; // liste de commandes -> dessins captés pas encore exécutés
// Une image du jeu à la fois (urti.ini frame_sync=, avec la caméra du jeu). Le jeu exécute ses listes sur une autre file que celle des
// présentations : à une présentation, il peut avoir commencé l'image suivante. La profondeur copiée à ce moment est à moitié dessinée
// (l'effet s'affaiblit d'un coup, une fois sur dix, caméra immobile), et une fenêtre de capture contient une image et demie ou deux.
// La profondeur est donc copiée dans la liste du jeu, à la fin de son écriture ; ce repère, à l'exécution, ferme la fenêtre : la suite
// (image suivante) attend le calcul suivant.
struct LiveDeferred
{
    std::vector<LivePending> pending;
    uint32_t listMain = 0, counted = 0;
};
bool g_liveFrameSync = false, g_liveTraceOn = false;
bool g_liveWindowClosed = false;
std::vector<LiveDeferred> g_liveDeferred;
int32_t g_liveSnapDone = -1;   // tampon de la dernière profondeur complète
uint64_t g_liveSnapSerial = 0; // nombre de profondeurs complètes
uint32_t g_liveSnapNext = 0, g_liveMainSinceMark = 0, g_liveMainLast = 0, g_liveMarksRefused = 0;
// dessins de la profondeur principale écrits, à la dernière copie : pas de nouvelle copie si rien n'a été écrit depuis (le jeu change
// l'état de sa profondeur une dizaine de fois par image, et six copies de suite reprenaient le tampon de la bonne)
uint64_t g_liveMainRecorded = 0, g_liveMainAtCopy = ~0ull;
// image de matériau (masque de l'eau) de la même image du jeu que la profondeur : copiée à part, un peu avant ou un peu après elle. Avec la
// copie de l'image d'avant, en mouvement, une feuille passait pour de l'eau (matériau du ciel, profondeur de la feuille) : un reflet parti au
// hasard, un point brillant, qui ne se voyait jamais jeu en pause
uint32_t g_liveMaterialNext = 0, g_liveMaterialSinceClose = 0;
int32_t g_liveMaterialDone = -1, g_liveWindowMaterial = -1;
uint32_t g_liveSnapCopies = 0;
// image du jeu sans son interface (Horizon avec DLSS-G : la cible que reçoit la génération d'images, alpha = forme de l'interface).
// Reconnue à l'exécution : la première image de la taille de l'écran en RGBA 16 bits flottants finie après le matériau de l'image
// (après la profondeur sans matériau) ; adoptée quand c'est la même ressource au moins 8 fois sur 10. Puis copiée à chaque image
uint64_t g_liveHudHandle = 0;
std::unordered_map<uint64_t, uint32_t> g_liveHudVotes;
uint32_t g_liveHudVoteCount = 0, g_liveHudNext = 0, g_liveHudW = 0, g_liveHudH = 0;
bool g_liveHudArmed = false;
int32_t g_liveHudDone = -1;
uint64_t g_liveHudDonePresent = 0, g_liveHudSerial = 0; // présentation et numéro de la dernière copie exécutée
uint64_t g_livePairSnap = 0, g_livePairHud = 0;         // dernière paire connue : profondeur complète (numéro) et copie sans interface de la même image du jeu
uint64_t g_liveWindow = 0;                                // fenêtre de présentation (avancée par urti_live.inl)
uint32_t g_liveAreaCursor[4][kLiveAreas] = {}, g_liveAreaProbe[4][kLiveAreas] = {}; // octets pris dans chaque partie (début, fin)
uint32_t g_liveAreaCounters[kLiveAreas] = {};             // compteurs pris dans chaque partie
uint32_t g_liveStaleDraws = 0;                            // dessins exécutés trop tard (partie du flux déjà reprise), ignorés
// signature du maillage sans le rang : budget et tailles apprises sont par maillage (tous ses exemplaires)
uint32_t MeshId(uint64_t idKey)
{
    const uint64_t h = (idKey ^ (idKey >> 31)) * 0x9E3779B97F4A7C15ull;
    return uint32_t(h ^ (h >> 32)) | 1u;
}
std::unordered_map<uint32_t, uint32_t> g_liveIdMesh; // identité -> maillage (cette image)
// surfaces transparentes de l'écran principal (eau, vitres ; aussi fumée, particules, écartées ensuite par
// live_planes) : captées à part, rangées à la suite dans leur propre couche de la scène RT, sans budget par maillage
constexpr uint32_t kLiveTranspVertices = 1500000;   // sommets de la couche (plafond par image)
std::vector<LiveRegion> g_liveTranspRegions;        // ordre d'exécution : début dans la couche, taille, compteur
std::vector<uint32_t> g_liveTranspSrc;              // même ordre : début de la zone écrite dans le flux
uint32_t g_liveTranspCursor = 0;                    // octets de la couche (16 par sommet)
uint64_t g_liveTranspVerts = 0;                     // sommets réservés dans cette fenêtre d'écriture
uint32_t g_liveTranspRefused = 0;                   // dessins refusés (indirects, couche pleine)
uint32_t g_liveTraceVertices = 0;                   // urti.ini trace_draw= : journal de chaque dessin de cette taille
int g_liveMainSlotIndex = -1;                       // emplacement de l'écran principal (dessins sans profondeur)
struct CensusEntry { uint64_t draws = 0, vertices = 0; std::map<uint32_t, uint32_t> sizes; };
std::map<uint32_t, CensusEntry> g_liveCensus;       // urti.ini motion_log=1 : tous les dessins de la scène, par classe
bool g_liveCensusOn = false;
bool g_liveTranspOn = true;                         // urti.ini water=0 : pas de couche transparente
// zone où le jeu dessine vraiment son image dans la profondeur de l'écran : la plus fréquente parmi les zones de
// dessin de ses dessins (bord droit, bord bas), cette fenêtre. Avec le DLSS et la résolution dynamique, Horizon
// Forbidden West dessine en 1280×720 à 1707×960 dans des cibles de 2560×1440 ; quelques dessins (repères en 3D)
// y gardent la zone pleine
std::unordered_map<uint32_t, uint32_t> g_liveViewCounts; // (largeur << 16 | hauteur) -> dessins
// budget de géométrie (urti_live.inl le met à jour à chaque image) : un objet déjà mesuré et peu important
// (petit, lointain) n'entre pas dans la scène RT ; ceux à re-mesurer sont captés dans la zone de mesure, à
// la fin de l'emplacement, hors de la structure d'accélération
std::vector<LiveRegion> g_liveProbes[4];
std::unordered_set<uint32_t> g_liveKnown, g_liveAdmit, g_liveProbeWant; // maillages
std::unordered_map<uint32_t, uint32_t> g_liveSeen; // maillages de l'écran dans cette image -> sommets (tous exemplaires)
// ExecuteIndirect : nombre de sommets inconnu avant le dessin ; on réservait 65 536 sommets à chacun (place
// vide prise dans l'emplacement et dans la structure). Taille apprise par maillage (mesurée), marge comprise.
std::unordered_map<uint32_t, uint32_t> g_liveLearned;
// dans RDR2 l'ordre des ExecuteIndirect change d'une image à l'autre (identité instable) : la taille est
// aussi apprise par signature de commande (le plus grand nombre de sommets écrits par ses dessins)
std::unordered_map<uint32_t, uint32_t> g_liveLearnedSig, g_liveIdSig; // signature -> sommets ; identité -> signature (cette image)
uint64_t g_liveReserved = 0; // sommets réservés dans l'écran principal (bilan)
uint64_t g_liveBudgetVertices = 0, g_liveProbeBudget = 0, g_liveProbeVerts = 0; // budget, zone de mesure (par image)
std::unordered_map<uint32_t, uint32_t> g_liveProbeStride, g_liveProbeDraws, g_liveProbePhase; // maillage énorme : un dessin capté sur N à chaque tour, la part de ce tour
std::unordered_set<uint32_t> g_liveStableNow; // maillages dont les sommets gardent leur ordre (dessin direct, une instance)
std::unordered_set<uint64_t> g_liveExecQueues; // files où des dessins du jeu ont été exécutés depuis le dernier calcul
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
std::vector<LiveRegion> g_liveRegions[4]; // par emplacement (kSlots), dans l'ordre d'exécution : place dans la scène RT
std::vector<uint32_t> g_liveRegionSrc[4]; // même ordre : début de la zone écrite dans le flux (octets)
uint32_t g_liveCursor[4] = {};            // sans places fixes : zones à la suite dans la scène RT (octets)
// place fixe de chaque objet de l'écran principal dans son emplacement, d'une image à l'autre : un objet que le
// jeu ne dessine pas pendant quelques images garde sa dernière géométrie dans la scène RT (plus de flash), et
// la disposition stable permet de mettre à jour la structure au lieu de la reconstruire (urti.ini persist=0 :
// zones attribuées à la suite à chaque image, comme avant)
struct LivePlace
{
    uint32_t offset = 0, capacity = 0, countIndex = 0, frameCounter = 0;
    uint64_t lastFrame = 0, prevFrame = 0; // dernière image où l'objet a été dessiné, et celle d'avant
    bool stableOrder = false;              // ses sommets gardent leur ordre (paires pour le mouvement de la caméra)
    // maillage dessiné une seule fois dans l'image précédente : la place est bien le même objet (Horizon trie ses
    // dessins de l'avant vers l'arrière : le n-ième exemplaire d'un maillage n'est pas le même d'une image à l'autre)
    bool uniquePrev = false;
    uint64_t rankKey = 0;
};
bool g_livePersist = true;
std::unordered_map<uint32_t, LivePlace> g_livePlaces; // identité -> place
std::map<uint32_t, uint32_t> g_liveFreeSpace;         // début -> taille (octets, multiples de 48 : trois sommets)
std::vector<uint32_t> g_liveFreeCounts;               // cases de compte rendues
uint32_t g_livePlaceHigh = 0, g_liveNextCount = 0;    // fin de la partie occupée (octets) ; cases de compte attribuées
uint64_t g_livePlaceFrame = 1;                         // image de capture (avancée par urti_live.inl)
uint32_t g_livePlaceNew = 0, g_livePlaceFailed = 0;    // places attribuées, refusées (emplacement plein) dans cette image

// rend la place : son espace (fusionné avec ses voisins libres) et sa case de compte
void LiveFreePlace(const LivePlace& p)
{
    uint32_t start = p.offset, size = p.capacity;
    if (auto next = g_liveFreeSpace.find(start + size); next != g_liveFreeSpace.end())
        size += next->second, g_liveFreeSpace.erase(next);
    if (auto prev = g_liveFreeSpace.lower_bound(start); prev != g_liveFreeSpace.begin())
        if (--prev; prev->first + prev->second == start)
            start = prev->first, size += prev->second, g_liveFreeSpace.erase(prev);
    if (start + size == g_livePlaceHigh)
        g_livePlaceHigh = start; // en haut de la partie occupée : elle rétrécit
    else
        g_liveFreeSpace[start] = size;
    g_liveFreeCounts.push_back(p.countIndex);
}
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
bool g_liveSunEighth = false; // ombres du soleil coupées : carte du soleil captée par huitièmes
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
    bool depthTested = false;   // profondeur testée (une surface transparente : eau, vitre, mais aussi fumée, décals)
    bool tessellated = false;   // tessellation (vagues de l'eau dans certains moteurs)
    bool blends = false;        // mélange à l'image sur la première cible (surface transparente : eau, vitre, fumée)
    bool transparentUsable = false; // surface transparente captée à part (couche 2 de la scène RT : reflets de l'eau)
    bool noDepthUsable = false;     // triangles sans test de profondeur (surface dessinée sans profondeur liée)
    bool otherStage = false;        // géométrie, tessellation ou mesh shader
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
            if (s.data && static_cast<const shader_desc*>(s.data)->code_size)
                other = true, info.tessellated = true;
            break;
        case pipeline_subobject_type::geometry_shader:
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
    // (mélangé ou non : l'eau d'Horizon Forbidden West calcule sa réfraction elle-même et écrit sa couleur d'un bloc ;
    // les dessins qui écrivent dans le G-buffer sont écartés à la capture)
    info.transparentUsable = !other && info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && info.rootSignature && info.depthTested && !info.writesDepth;
    info.noDepthUsable = !other && info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && info.rootSignature && !info.depthTested;
    info.otherStage = other;
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
// direct : trois parties tournantes par emplacement (une par fenêtre d'écriture), compteurs par partie
constexpr uint64_t kLiveAreaBytes = kSlotBytes / kLiveAreas / 48 * 48;
constexpr uint32_t kLiveAreaCounters = (kMaxDraws - kSlots) / kLiveAreas;

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
    uint64_t indexBuffer = 0, vertexBuffer = 0; // tampons liés (ressource et position mêlées) : identité des ExecuteIndirect
    uint64_t backBuffer = 0; // image d'écran liée en cible (photos avant l'interface à ses premiers dessins)
    uint32_t backBufferDraws = 0; // dessins dans l'image d'écran depuis sa liaison
    uint64_t gbufferSet = 0; // jeu de cibles candidat au G-buffer lié (sa première cible), 0 : aucun
    float viewport[4] = {}; // x, y, largeur, hauteur (le premier)
    // présentation pendant laquelle la liste a reçu son premier dessin, dessins depuis sa remise à zéro : une liste
    // écrite avant une présentation et exécutée après (moteurs qui préparent l'image suivante en avance) est vue
    uint64_t window = ~0ull;
    uint32_t draws = 0;
    uint64_t drewBackBuffer = 0; // image d'écran où cette liste a dessiné (image réelle ou générée : voir OnPresent)
    uint32_t mainDraws = 0;      // dessins dans la profondeur principale écrits dans cette liste
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
    float slotScore[kSlots] = {}; // dessins des images précédentes (moyenne glissante) : choix de l'écran principal
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

// tampons d'indices et de sommets liés : ce qui reste stable d'une image à l'autre pour un ExecuteIndirect (le
// tampon de ses arguments tourne dans RDR2 : une identité qui en dépend change à chaque image)
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
    st.backBufferDraws = 0;
    st.gbufferSet = 0;
    if (targets[0])
        if (auto it = g.screenLike.find(depth); it != g.screenLike.end() && it->second == 1)
        {
            // une même première cible liée avec moins de cibles (dans RDR2, trois au lieu de cinq pour une partie
            // des objets) : la liaison la plus large est gardée (sinon les vecteurs de mouvement, cinquièmes, partaient)
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

// état de départ des profondeurs (une carte d'ombres peut rester en écriture sans jamais de barrière)
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

// rejoue le dessin avec le pipeline ombre ; le dessin du jeu a lieu ensuite, inchangé
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
    bool transp = false; // surface transparente (eau, vitre…) : couche à part de la scène RT
    bool noDepth = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        state = g.lists[(uint64_t)list];
        if (state.gbufferSet)
            if (auto set = g_gbufferSets.find(state.gbufferSet); set != g_gbufferSets.end())
                set->second.draws++;
        slot = SlotFor(state.depth);
        auto it = g_pipelines.find(state.pipeline);
        // surface transparente (couche 2 de la scène RT) : sur la profondeur principale, profondeur testée sans être
        // écrite, hors du G-buffer choisi (le passage principal d'un moteur avec passe de profondeur préalable redessine
        // toute la scène ainsi dans le G-buffer) ; ou tout dessin sur une autre vue de la caméra (même forme que l'écran,
        // résolution réduite) : Horizon Forbidden West dessine son eau à mi-résolution, dans ses propres images et sa
        // propre profondeur. live_planes ne garde que les surfaces planes ; live_reflect, que celles qui ont de l'épaisseur
        // au-dessus de ce que montre la profondeur (pas les décalques posés sur le sol)
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
        // (les dessins sans profondeur d'Horizon sont des nuages de brume : pas captés pour l'instant)
        if (g_liveTraceVertices && kind != 2 && params[0] * std::max(1u, params[1]) == g_liveTraceVertices)
            Log("TRACE dessin %u : genre %u profondeur %llx (principale %llx) forme %d flux %d utilisable %d transparent %d phase %d fenetre %llu", g_liveTraceVertices, kind, (unsigned long long)state.depth,
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
        // mode direct : tout le flux de l'écran (structure d'accélération) ; de la carte d'ombres du
        // soleil, seulement des dessins marqués ; à l'écran, marqués ceux qui ont un pendant dans la carte
        const bool screen = noDepth || g.screenLike[state.depth] == 1;
        bool sunMap = g_liveSunDepth && state.depth == g_liveSunDepth;
        if (!screen && !sunMap)
            return;
        uint64_t key = kind * 1000003ull;
        for (uint32_t p : params)
            key = key * 1315423911ull + p;
        // ombres du soleil coupées (Horizon : occultation seule) : la carte du soleil ne sert qu'à mesurer la direction du soleil, qui change lentement ;
        // un huitième de ses dessins à chaque image, à tour de rôle (elle faisait 70 % des sommets rejoués : 1 million par présentation)
        if (sunMap && !screen && g_liveSunEighth && ((key * 0x9E3779B97F4A7C15ull) >> 61) != (g_liveWindow & 7))
            return;
        // taille de la zone : sommets connus du dessin (listes de triangles) ; indirect : budget fixe
        // arrondie à trois sommets : la structure d'accélération forme les triangles par triplets depuis le
        // début du flux ; une zone d'un autre multiple décalerait toutes les suivantes (triangles géants
        // reliant des objets différents)
        uint64_t vertices = (kind == 2 ? 65536ull : uint64_t(params[0]) * std::max(1u, params[1]) + 2) / 3 * 3;
        LivePending pend;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const bool mainScreen = screen && state.depth == g_liveMainDepth;
            uint64_t idKey = kind * 1000003ull;
            uint32_t mesh = 0;
            bool probe = false;
            uint32_t part = 0;
            // surface transparente : seulement sur l'écran principal, sans identité ni budget par maillage, dans la
            // limite de la couche
            // sur une autre vue, ils comptent pour le choix de l'écran principal : sinon, une fois une mauvaise profondeur
            // choisie (l'eau d'Horizon, 15 dessins, juste après le menu), la vraie, dont tous les dessins passent ici, ne
            // reprenait jamais la main
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
                // identité stable d'une image à l'autre : le maillage (nombre d'indices ou de sommets, début dans
                // ses tampons), pas le nombre d'instances ni la première instance (une végétation dessinée par
                // instances en change à chaque image : dans RDR2, 120 000 identités pour 800 objets à l'écran) ;
                // un ExecuteIndirect : sa signature de commande, son pipeline, les tampons d'indices et de sommets
                // liés, son nombre maximal de commandes ; pas son tampon d'arguments, qui tourne dans RDR2 (une
                // identité par image : 107 000 mesures en mémoire pour 900 objets, jamais connus, la scène RT
                // tirée au hasard parmi eux à chaque image)
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
                // taille apprise de ce maillage (une même signature sert à des dessins de toutes tailles : son
                // maximum imposait 65 536 sommets à tous)
                if (kind == 2)
                    if (const auto jt = g_liveLearned.find(mesh); jt != g_liveLearned.end())
                        vertices = std::min<uint64_t>(65536, (uint64_t(jt->second) * 5 / 4 + 64 + 2) / 3 * 3);
                // tous les dessins comptent pour choisir l'écran principal, même hors budget (sinon une autre
                // profondeur, moins chargée, finirait par passer devant)
                g.slotDraws[slot]++;
            }
            if (mainScreen && !transp)
            {
                g_liveSeen[mesh] += uint32_t(vertices);
                g.lists[uint64_t(list)].mainDraws++;
                g_liveMainRecorded++;
                if (g_liveBudget && !g_liveAdmit.count(mesh))
                {
                    // hors des maillages choisis : un inconnu entre tant que la place du budget le permet ; sinon
                    // (et pour un maillage connu dont la mesure est à refaire) il est mesuré dans la zone de mesure,
                    // dans sa limite par image ; le reste n'est pas capté du tout
                    const bool known = g_liveKnown.count(mesh) != 0;
                    if (known || g_liveReserved + vertices > g_liveBudgetVertices)
                    {
                        if ((known && !g_liveProbeWant.count(mesh)) || g_liveProbeVerts + vertices > g_liveProbeBudget)
                        {
                            g_liveOutOfBudget++;
                            return;
                        }
                        // maillage énorme (herbe, feuillage : des milliers de dessins) : un dessin sur N à chaque tour, pas
                        // toujours les mêmes. Chaque part a ses identités : avec le seul rang parmi les dessins captés, la part
                        // de ce tour écrasait dans le cache celle du tour d'avant, herbe et feuillage y apparaissaient par
                        // morceaux, sans cesse remplacés
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
            // zone dans la partie du flux de cette fenêtre d'écriture : les dessins depuis son début, la zone de
            // mesure depuis sa fin
            const uint32_t area = uint32_t(g_liveWindow % kLiveAreas);
            const uint32_t capacity = uint32_t(std::min<uint64_t>(vertices * 16, kLiveAreaBytes / 2));
            const uint32_t size = (capacity + 15) & ~15u;
            uint32_t &cursor = g_liveAreaCursor[slot][area], &probeUsed = g_liveAreaProbe[slot][area];
            if (!capacity || uint64_t(cursor) + probeUsed + size > kLiveAreaBytes || g_liveAreaCounters[area] >= kLiveAreaCounters)
            {
                if (screen && !probe)
                    g_liveOverflow++;
                return; // partie pleine : ce dessin n'est pas capté
            }
            // cache de géométrie : un quart de la partie reste aux dessins de cette image (l'eau arrive tard dans l'image
            // du jeu : la zone pleine de gros objets du cache, elle n'était pas captée une fois sur deux)
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

// photos du contenu de l'image d'écran avant ses 1er, 2e, 8e et 32e dessins depuis sa liaison : dans RDR2, le
// premier dessin écrit lui-même l'image de la scène (photo prise avant : l'image précédente, une fois sur deux
// environ) ; live_uiselect garde la plus tôt qui contient déjà l'image courante
void LiveUiSnapshot(command_list* cmd)
{
    ID3D12Resource *target = nullptr, *snap = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g.lists.find(cmd->get_native());
        if (it == g.lists.end() || !it->second.backBuffer)
            return;
        // avant le premier dessin (contenu laissé là : dans RDR2 une image précédente, interface comprise) et
        // avant le deuxième (le premier a écrit l'image de la scène, pas encore l'interface) ; seulement dans une
        // liste de commandes directe : une copie est interdite dans les autres (les « bundles » de l'interface),
        // en prendre au 8e et 32e dessins faisait planter RDR2 dès son menu (v56)
        if (it->second.backBufferDraws++ > 1)
            return;
        if (ID3D12GraphicsCommandList* native = Native(cmd); !native || native->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return;
        if (g_liveSnapCount >= kLiveSnaps || !g_liveSnap[g_liveSnapCount])
            return;
        target = reinterpret_cast<ID3D12Resource*>(it->second.backBuffer);
        // image d'écran recréée (passage en HDR, Alt+Tab, changement de taille) : la photo n'est prise que si elle
        // a la disposition des tampons (une copie d'un autre format ou d'une autre taille rend la liste du jeu
        // invalide : Horizon Forbidden West plantait au passage en HDR)
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

// un dessin de plus dans cette liste (fenêtre de présentation de son premier dessin)
void NoteDraw(uint64_t list)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CommandListState& st = g.lists[list];
    if (st.draws++ == 0)
        st.window = g_presentIndex.load(std::memory_order_relaxed);
    if (st.backBuffer)
        st.drewBackBuffer = st.backBuffer;
}

// liste remise à zéro sans avoir été exécutée : ses dessins captés ne seront jamais remplis
void OnResetCommandList(command_list* cmd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const auto it = g.lists.find(cmd->get_native()); it != g.lists.end())
        it->second.draws = 0, it->second.window = ~0ull, it->second.drewBackBuffer = 0, it->second.mainDraws = 0;
    g_livePending.erase(cmd->get_native());
}

// dessins captés d'une liste soumise à la file du jeu : ils appartiennent à la présentation en cours (rang,
// identité, place dans la scène RT), quelle que soit la fenêtre où la liste a été écrite (appelé sous g_mutex)
// repères de matériau d'une liste : comptés dès son exécution, même si ses dessins attendent le calcul suivant (ils sont retirés de la liste)
void LiveNoteMaterial(std::vector<LivePending>& pending)
{
    for (auto it = pending.begin(); it != pending.end();)
    {
        if (it->hudHandle)
        {
            // image de la taille de l'écran finie : la première après le matériau est l'image sans interface
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
                // le repère reste à sa place : lu dans l'ordre d'exécution avec les fins de profondeur (LiveCommit), même si la liste attend le calcul suivant
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
            g_liveWindowMaterial = it->materialMark; // image fermée, son matériau vient après sa profondeur
        else
            g_liveMaterialSinceClose++;
        if (g_liveTraceOn)
            Log("MATIERE tampon %d fenetre fermee %d", it->materialMark, int(g_liveWindowClosed));
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
            // fin d'écriture de la profondeur. Plusieurs par image possibles : seule compte celle qui a vu au moins 60 % des dessins
            // de l'image précédente (sinon la profondeur n'est pas finie)
            g_liveMainSinceMark += p.mainBefore - counted;
            counted = p.mainBefore;
            const bool complete = g_liveMainSinceMark >= 16 && uint64_t(g_liveMainSinceMark) * 10 >= uint64_t(g_liveMainLast) * 6;
            if (g_liveTraceOn)
                Log("REPERE tampon %d dessins %u (image d'avant %u) etat %x %s", p.depthMark, g_liveMainSinceMark, g_liveMainLast, p.depthAfter, complete ? "complet" : "refuse");
            if (!complete)
            {
                g_liveMarksRefused++;
                continue;
            }
            g_liveMainLast = g_liveMainSinceMark, g_liveMainSinceMark = 0;
            g_liveSnapDone = p.depthMark, g_liveSnapSerial++;
            if (g_liveMaterialNext == 0)
                g_liveHudArmed = true; // pas de matériau copié : l'image sans interface est la première finie après la profondeur
            g_liveWindowClosed = true;
            // matériau déjà passé dans cette image : c'est le sien ; sinon, le prochain
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
            // copie de l'image sans interface : celle de l'image du jeu dont la profondeur vient d'être complète
            g_livePairSnap = g_liveSnapSerial, g_livePairHud = p.hudSerialMark;
            continue;
        }
        // partie du flux déjà reprise par des dessins plus récents (liste exécutée trois fenêtres après son
        // écriture) : ignoré
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
            // rang parmi les dessins de même signature, dans l'ordre d'exécution (le même d'une image à l'autre)
            const uint64_t h = (p.rankKey ^ (p.rankKey >> 29)) * 0x9E3779B97F4A7C15ull + g_liveKeyRank[p.rankKey]++ + (uint64_t(p.part) << 24);
            id = uint32_t(h ^ (h >> 32)) | 1u; // 0 : pas d'identité
            if (p.indirect)
                g_liveIdSig[id] = p.sig;
            if (p.mainScreen)
            {
                const uint32_t meshNow = MeshId(p.rankKey);
                g_liveIdMesh[id] = meshNow;
                if (p.stableOrder)
                    g_liveStableNow.insert(meshNow);
                // zone de dessin, comptée à l'exécution (dans l'ordre d'écriture, deux images s'y mêlaient)
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
            // sa place s'il en a une assez grande (et pas déjà prise dans cette image), sinon une nouvelle :
            // le premier espace libre qui convient, sinon au bout de la partie occupée
            auto it = g_livePlaces.find(id);
            if (it != g_livePlaces.end() && it->second.lastFrame == g_livePlaceFrame)
                continue; // même identité deux fois dans l'image : la seconde n'est pas gardée
            if (it != g_livePlaces.end() && it->second.capacity < p.capacity)
                LiveFreePlace(it->second), g_livePlaces.erase(it), it = g_livePlaces.end();
            if (it == g_livePlaces.end())
            {
                // sans marge : la structure est reconstruite à chaque image, une place redistribuée ne coûte rien,
                // et chaque sommet de trop (marge, trou) alourdit la reconstruction
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
                        continue; // scène RT pleine : pas gardé (les places sont redistribuées ensuite)
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

// liste soumise à une file : ses dessins comptent pour la présentation en cours, pas pour celle où ils ont été écrits
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
            // fenêtre fermée : cette liste appartient à l'image suivante
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

// copyOf : un tampon de même disposition (copie de l'image, dans l'état copyState) enregistré à la place de l'image
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
    // première image d'une rafale : texels bruts à côté (même pas d'échantillonnage), pour comparer les codages (image sans interface du jeu)
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
        LiveLoadCalibration(); // avant toute décision d'étalonnage
    // images par seconde, journalisées toutes les 300 présentations (avec ou sans ombres RT)
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
            Log("URTI : %.1f images par seconde (ombres RT %s)", count / sum, live.enabled ? "actives" : "coupées");
            // temps entre présentations : médiane et pics (P95, P99), et coût de la capture sur ces 300 présentations
            std::sort(gaps, gaps + count);
            const double tick = 1000.0 / double(freq.QuadPart);
            Log("URTI : présentations en ms : médiane %.2f, P95 %.2f, P99 %.2f, pire %.2f ; capture : %.2f ms de processeur par présentation, %.0f dessins vus, "
                "%.0f rejoués (%.0f k sommets), dont zone de mesure %.0f (%.0f k sommets)",
                gaps[count / 2] * 1000.0, gaps[count * 95 / 100] * 1000.0, gaps[count * 99 / 100] * 1000.0, gaps[count - 1] * 1000.0,
                double(g_capTicks.exchange(0)) * tick / count, double(g_capCalls.exchange(0)) / count, double(g_capReplayed.exchange(0)) / count,
                double(g_capReplayVerts.exchange(0)) / count / 1000.0, double(g_capProbeReplayed.exchange(0)) / count, double(g_capProbeVerts.exchange(0)) / count / 1000.0);
            Log("URTI : sommets rejoués par présentation (k) : écran principal %.0f, carte du soleil %.0f, transparents %.0f, autres écrans %.0f",
                double(g_capMainVerts.exchange(0)) / count / 1000.0, double(g_capSunVerts.exchange(0)) / count / 1000.0, double(g_capTranspVerts.exchange(0)) / count / 1000.0,
                double(g_capOtherVerts.exchange(0)) / count / 1000.0);
            sum = 0, count = 0;
        }
    }
    if (GetAsyncKeyState(VK_F7) & 1)
    {
        live.enabled = !live.enabled;
        Log("URTI : ombres RT en direct %s", live.enabled ? "actives" : "coupées");
    }
    g_presentDraws.exchange(0);
    // image générée : aucun dessin du jeu exécuté depuis la présentation précédente (écrits : Decima écrit souvent
    // ceux de l'image suivante avant de présenter, une image sur six semblait générée)
    live.lastPresentDraws = g_presentExecDraws.exchange(0);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& pq = g_diagPresentQueues[queue->get_native()];
        pq.first++, pq.second += live.lastPresentDraws ? 1 : 0;
        g_diagPattern = (g_diagPattern << 1) | (live.lastPresentDraws ? 1 : 0);
        // image générée (DLSS-G, FSR3) : le jeu n'a pas dessiné dans l'image présentée depuis la présentation
        // précédente (l'image fabriquée est écrite par la génération, hors des listes du jeu). Quand le jeu ne dessine
        // jamais dans l'image d'écran (Horizon Forbidden West avec DLSS-G : il dessine dans une image intermédiaire de
        // Streamline), l'alternance des présentations en tient lieu : une génération par interpolation présente
        // l'image fabriquée avant la vraie, donc celle qui suit les dessins du jeu est la fabriquée, la suivante (sans
        // dessin exécuté) la vraie. Sans alternance, une présentation sans dessin reprend simplement l'image précédente
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
                std::snprintf(item, sizeof(item), " file %04llx : %u (%u avec dessins)", (unsigned long long)(q & 0xFFFF), c.first, c.second);
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
            Log("URTI direct : présentations par file :%s ; dessins exécutés par file :%s ; suite (dessins exécutés) %s ; suite (image dessinée par "
                "le jeu) %s ; dessins ignorés (exécutés trop tard) %u", text, exec, bits, bbBits, g_liveStaleDraws);
            Log("URTI direct : écran principal, ignorés sur 300 présentations : %llu dessins transparents (%llu sommets, le plus gros %llu), "
                "%llu dessins tessellés (%llu sommets)", (unsigned long long)g_diagTranspDraws, (unsigned long long)g_diagTranspVerts,
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
            Log("URTI direct : dont mélangés à l'image (transparents) : %llu dessins, %llu sommets ; plus gros (sommets × dessins) :%s",
                (unsigned long long)g_diagBlendDraws, (unsigned long long)g_diagBlendVerts, sizes);
            g_diagTranspDraws = g_diagTranspVerts = g_diagTranspMax = g_diagTessDraws = g_diagTessVerts = 0;
            g_diagBlendDraws = g_diagBlendVerts = 0, g_diagBlendSizes.clear();
            g_diagPresentQueues.clear(), g_diagExecQueues.clear(), g_liveStaleDraws = 0;
        }
    }
    // G-buffer : le jeu de cibles le plus dessiné, revu toutes les 30 images (tout de suite s'il n'y en a pas)
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
            Log("URTI direct : G-buffer : %u cibles (formats %s), %llu dessins (%zu jeux de cibles candidats)", best->count, formats,
                (unsigned long long)best->draws, g_gbufferSets.size());
            std::memcpy(g_liveGbuffer, best->targets, sizeof(g_liveGbuffer));
            g_liveGbufferCount = best->count;
        }
        if (best || g.frame % 30 == 0)
            for (auto it = g_gbufferSets.begin(); it != g_gbufferSets.end();)
                it = it->second.draws ? (it->second.draws = 0, ++it) : g_gbufferSets.erase(it); // plus dessiné : ressources peut-être détruites
    }
    if (GetAsyncKeyState(VK_F6) & 1)
        live.forceCalibration = true;
    if (GetAsyncKeyState(VK_F5) & 1)
        live.toggleSplit = true;
    // F4 : avant / après en plein écran, instantané (seule la pose sur l'image est sautée, le calcul continue)
    if (GetAsyncKeyState(VK_F4) & 1)
    {
        live.showGameOnly = !live.showGameOnly;
        Log("URTI direct : F4, %s", live.showGameOnly ? "image du jeu seule (avant)" : "image avec URTI (après)");
    }
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
    // film au rythme réel (fichier déclencheur urti_film) : 30 présentations consécutives, générées comprises, gardées en mémoire vidéo
    if (const std::string film = g_dir + "\\urti_film"; live.film == 0 && live.burst == 0 && g.frame % 30 == 0 && GetFileAttributesA(film.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA(film.c_str());
        live.filmFolder = g_dir + "\\urti_film_" + std::to_string(++live.filmCount);
        CreateDirectoryA(live.filmFolder.c_str(), nullptr);
        live.film = LiveState::kFilmFrames, live.filmIndex = 0;
        Log("URTI direct : film %u demandé (%u présentations au rythme réel)", live.filmCount, LiveState::kFilmFrames);
    }
    g_liveCapture = live.enabled && g.phase == Phase::Idle;
    // étalonnage complet (lourd : tout le flux relu) au départ, sur F6, ou quand le suivi du soleil
    // image par image est perdu ; sinon le soleil est suivi sur le GPU. Sans ombres RT du soleil (réglage par
    // défaut) et avec la loi de profondeur connue (mesurée aussi à chaque image), il ne sert à rien : dans Horizon
    // Forbidden West, sans carte d'ombres reconnue, il recommençait toutes les 480 images et la capture de la scène
    // RT s'arrêtait pendant ce temps (l'effet disparaissait puis revenait toutes les cinq secondes)
    const bool calibrationUseful = live.sunShadows || !live.depthReady;
    if (live.enabled && g.phase == Phase::Idle && g.nextCapture == ~0ull &&
        (live.forceCalibration || (!live.calibrated && calibrationUseful && g.frame >= live.nextCalibration)))
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

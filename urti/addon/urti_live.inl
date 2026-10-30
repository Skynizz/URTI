// URTI en direct (inclus dans urti_addon.cpp) : ombres du soleil en ray tracing à chaque image.
//
// À chaque image : les dessins sur la profondeur de l'écran sont rejoués dans le flux (comme une
// capture) ; avant les effets de ReShade, sur sa liste de commandes : sommets vers l'espace caméra,
// structure d'accélération, un rayon par pixel vers le soleil, image d'ombres lue par l'effet urti.fx.
// La loi de profondeur et la carte d'ombres du soleil viennent d'un étalonnage (capture complète,
// cartes d'ombres comprises) fait à l'activation et sur F6 ; ensuite la direction du soleil est
// mesurée à chaque image sur le GPU (paires de dessins écran / carte d'ombres), sans relecture.
//
// Touches : F7 active / coupe ; F6 refait l'étalonnage. Fichier urti_photo à côté de l'add-on : image de
// l'écran après les effets (urti_photoN.ppm).

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

struct LiveState
{
    bool enabled = false, calibrated = false, bound = false;
    bool depthReady = false; // loi de profondeur connue (reflets) ; calibrated : soleil connu (ombres)
    // étalonnage stocké (urti_calib.ini) : propre au jeu, relu au lancement -> ombres dès la première image
    bool fromStore = false, verified = false, storeLoaded = false;
    uint32_t sunMapWidth = 0, sunMapHeight = 0, sunMapFormat = 0;
    float sun[3] = { 0, 1, 0 }, A = 0, B = 0.1f, p00 = 1, p11 = 1;
    uint64_t nextCalibration = 0;
    uint32_t calibrationInterval = 180;

    ComPtr<ID3D12RootSignature> convertRs, shadowRs;
    ComPtr<ID3D12PipelineState> convertPso, shadowPso;
    ComPtr<ID3D12Resource> view, blas, blasScratch, tlas, tlasScratch, instances, depthBuf, shadowBuf, mask, counterReadback[3];
    ComPtr<ID3D12Resource> prevShadowBuf; // masque de l'image réelle précédente (images générées : mélange)
    ComPtr<ID3D12Resource> shadowRawBuf;  // masque brut de cette image (avant débruitage)
    ComPtr<ID3D12RootSignature> denoiseRs;
    ComPtr<ID3D12PipelineState> denoisePso;
    // budget de géométrie : chaque objet capté est mesuré (distance, taille) ; seuls les plus importants
    // entrent dans la scène RT, jusqu'à vertexBudget sommets ; les autres sont re-mesurés par roulement
    // (probeBudget sommets par image, hors de la structure)
    // photo d'avant l'interface : photos de l'écran (urti_addon.cpp), comparaison, choix
    ComPtr<ID3D12Resource> snaps[kLiveSnaps], uiCounts, uiReadback[3];
    ComPtr<ID3D12RootSignature> uiRs;
    ComPtr<ID3D12PipelineState> uiPso;
    uint64_t snapBytes = 0, uiFrame = 0;
    uint32_t uiCountAt[3] = {};
    int uiChoice = -1; // rang de la photo d'avant l'interface (-1 : aucune sûre)
    bool uiPhoto = true; // urti.ini ui_photo=0 : sans la photo d'avant l'interface (contrôle)
    ComPtr<ID3D12RootSignature> statsRs;
    ComPtr<ID3D12PipelineState> statsPso;
    ComPtr<ID3D12Resource> statsBuf, statsUpload[3], statsReadback[3];
    uint64_t statsBytes = 0;
    std::vector<uint32_t> statsIds[3];
    struct ObjectStat
    {
        float importance;  // taille / distance (taille apparente)
        uint64_t frame;    // image de la dernière mesure
        uint32_t vertices; // sommets réellement écrits (mesurés)
    };
    std::unordered_map<uint32_t, ObjectStat> objects;
    std::unordered_map<uint32_t, uint32_t> seenNow;
    std::unordered_set<uint32_t> admitted;
    uint64_t vertexBudget = 4000000, probeBudget = 1000000, slotLimit = 0;
    uint64_t statDraws = 0, statOverflow = 0, statOutOfBudget = 0, statProbes = 0, statBudgetFrames = 0, statReserved = 0;
    // accumulation dans le temps : vecteurs de mouvement du jeu (G-buffer), profondeur précédente
    ComPtr<ID3D12Resource> mvBuf, depthPrevBuf, mvErrors, mvErrorsReadback[3];
    ComPtr<ID3D12RootSignature> mvcalRs;
    ComPtr<ID3D12PipelineState> mvcalPso;
    uint64_t mvBytes = 0, mvFrame = ~0ull;
    uint32_t mvPitch = 0, mvWidth = 0, mvHeight = 0, mvConvention = 255, mvMovingFrames = 0;
    double mvErr[16] = {}, mvCount[16] = {};
    float temporalAlpha = 0.3f;
    float reflAlphaMin = 0.1f, reflAlphaMax = 0.5f; // reflets : part de l'image courante, à l'arrêt et en mouvement rapide
    float reflGamma = 1.0f, reflDepthTol = 0.1f;    // reflets : largeur du bornage (écarts-types), tolérance de profondeur
    uint32_t debugView = 0;    // vue de diagnostic (menu UrtiDebug ou urti.ini debug_view=)
    int iniDebugView = -1;
    // reflets stabilisés : historique à la résolution de rendu (deux tampons alternés)
    ComPtr<ID3D12RootSignature> resolveRs;
    ComPtr<ID3D12PipelineState> resolvePso;
    ComPtr<ID3D12Resource> reflHist[2];
    uint64_t reflHistBytes = 0;
    int reflHistLast = -1; // tampon écrit à la dernière image réelle
    uint32_t fgMode = 0;                  // images générées : 0 même masque, 1 sans RT, 2 mélange des deux dernières
    // stabilité du soleil : écart d'une image mesurée à la suivante
    float lastSun[3] = {};
    double sunJitterSum = 0, sunJitterMax = 0;
    uint32_t sunJitterCount = 0;
    uint64_t capacity = 0; // sommets de la structure
    uint32_t width = 0, height = 0, depthPitch = 0, shadowPitch = 0;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    resource_view maskSrv = {};
    uint64_t frame = 0, lastFilled = 0;
    // structure d'accélération : mise à jour (refit, plusieurs fois moins chère) tant que la liste des
    // dessins est la même qu'à la dernière construction, reconstruite au plus tous les blasRefit images
    uint64_t blasLayout = 0;
    uint32_t blasAge = ~0u, blasRefit = 8, statRebuilds = 0, statUpdates = 0;
    uint64_t statVertices = 0;
    std::vector<ComPtr<ID3D12Resource>> retired[3]; // libérées trois images plus tard
    uint32_t photoIndex = 0;
    // mémoire géométrique : sommets de l'image précédente (espace caméra de cette image-là) et, par
    // identité d'objet, où commençait sa zone ; un point touché retrouve ainsi sa position précédente,
    // déformations comprises (vent, animation : les sommets sont captés après le vertex shader)
    ComPtr<ID3D12Resource> viewPrev, linkUpload[3], anchor[2];
    std::unordered_map<uint32_t, uint32_t> prevStart, pendingStart;
    bool prevValid = false;
    int anchorOut = -1; // ancrage écrit à cette image (anchor[anchorOut]), l'autre est celui de la précédente
    uint64_t anchorFrame = ~0ull;
    bool anchorPrevOk = false; // l'autre tampon a bien été écrit à l'image précédente
    uint64_t anchorBytes = 0;
    // ancrage coupé par défaut : mesuré sur le bac à sable, il ne réduit pas le clignotement (le passage
    // d'un filtre à l'autre selon les pixels en ajoute) ; anchor_alpha=0.1 dans urti.ini pour l'essayer
    float anchorAlpha = 0.0f, anchorGamma = 2.5f;
    bool alphaScreen = true; // trous des plaques découpées reconnus à l'écran (urti.ini alpha_screen=0 : coupé)
    uint64_t boundFrame = 0;

    // suivi du soleil à chaque image
    bool forceCalibration = false, sunDirty = false;
    int iniCombine = -1; // urti.ini combine= : 0 ou 1 impose le mode ; -1 : menu de ReShade (UrtiMode)
    std::atomic<bool> overlayOpen{ false }; // menu de ReShade ouvert : pas de passe RT (le menu doit rester visible)
    uint32_t shadowRays = 4;                // urti.ini shadow_rays= (1 à 4)
    // minutage GPU par étape (horodatages), moyenné sur 300 images
    ComPtr<ID3D12QueryHeap> timeHeap;
    ComPtr<ID3D12Resource> timeReadback[3];
    double timeSum[8] = {};
    uint32_t timeFrames = 0;
    uint32_t lastPresentDraws = 1; // dessins du jeu avant cette présentation (0 : image générée)
    // reflets : matériau du G-buffer copié à chaque image, rayons réfléchis
    ComPtr<ID3D12RootSignature> reflectRs, depthfitRs;
    ComPtr<ID3D12PipelineState> reflectPso, depthfitPso;
    ComPtr<ID3D12Resource> materialBuf, reflectBuf;
    uint64_t materialBytes = 0, materialFrame = ~0ull, reflectBytes = 0;
    uint32_t materialPitch = 0, materialWidth = 0, materialHeight = 0, materialFormat = 0;
    float reflStrength = 1.0f, smoothMin = 0.5f, f0 = 0.04f, reflEffective = 1.0f;
    uint32_t reflChannel = 1, reflInvert = 0, debugReflect = 0, materialSlot = 2, reflEverywhere = 0;
    uint64_t maskFrame = ~0ull;    // image où le masque d'ombres a été calculé (sinon il ne suit plus la caméra)
    int lastMode = -1;
    float lastStrength = -1;
    bool lastSplit = false, lastShowMask = false, lastTechnique = true;
    std::atomic<bool> toggleSplit{ false }; // F5 : écran partagé (gauche jeu seul, droite avec ombres RT)
    float sunSign = 1; // sens du soleil par rapport à la profondeur des cartes (convention du jeu)
    uint32_t missedCalibrations = 0;
    ComPtr<ID3D12RootSignature> fitRs, voteRs;
    ComPtr<ID3D12PipelineState> fitPso, votePso;
    ComPtr<ID3D12Resource> pairsUpload[3], pairDirs, sunBuf, sunUpload, sunReadback[3];
    ComPtr<ID3D12Resource> regionUpload[3]; // table des zones de l'écran pour le GPU (par image)
    ComPtr<ID3D12Resource> atlasBuf; // copie de la carte d'ombres du jeu, pour recalculer son ombre
    // application à l'image présentée (toutes les présentations, images générées comprises)
    ComPtr<ID3D12RootSignature> compositeRs;
    ComPtr<ID3D12PipelineState> compositePso;
    ComPtr<ID3D12Resource> imageIn, imageOut;
    uint64_t imageBytes = 0;
    // bilan par 300 présentations : sans dessin du jeu, effets de ReShade, ombres appliquées
    uint32_t statPresents = 0, statNoDraw = 0, statEffects = 0, statComposited = 0;
    // rechargement à chaud : réglages (urti.ini) et shaders (urti_shaders\*.cso), sans relancer le jeu
    FILETIME iniTime = {}, shaderTime[3] = {};
    std::vector<ComPtr<ID3D12PipelineState>> retiredPso[3];
    uint64_t atlasBytes = 0;
    uint32_t atlasPitch = 0;
    bool atlasD24 = false;
    uint64_t framesSinceSun = 0, sunFrames = 0, sunAccepted = 0, pairSum = 0;
    uint32_t rotationValid = 0;
    double rotationSum = 0;
} live;
constexpr uint32_t kLiveMaxPairs = 256;
// soleil, bilan, cascades (48 + 64 k), soleil en attente (560), rotation de la caméra (576), cascades de
// l'image précédente (640)
constexpr uint32_t kLiveSunBytes = 656 + 8 * 64;

// ------------------------------------------------------------------------------------------------
// étalonnage stocké : loi de profondeur, convention de la carte d'ombres, description de cette carte
// ------------------------------------------------------------------------------------------------

void LiveSaveCalibration()
{
    if (FILE* f = std::fopen((g_dir + "\\urti_calib.ini").c_str(), "w"))
    {
        std::fprintf(f, "; étalonnage URTI de ce jeu (relu au lancement ; F6 le refait)\n");
        std::fprintf(f, "depth_a=%.9g\ndepth_b=%.9g\nsun_sign=%d\nsun_map=%ux%u:%u\nmotion=%u\n", live.A, live.B, live.sunSign > 0 ? 1 : -1, live.sunMapWidth,
                     live.sunMapHeight, live.sunMapFormat, live.mvConvention);
        std::fclose(f);
    }
}

// projection supposée (champ de vision arbitraire, comme Calibrate) pour le rapport largeur / hauteur
void LiveSetProjection(const D3D12_RESOURCE_DESC& depthDesc)
{
    const float fov = 50.0f * 3.14159265f / 180.0f;
    const float aspect = depthDesc.Height ? float(depthDesc.Width) / float(depthDesc.Height) : 16.0f / 9.0f;
    live.p11 = 1.0f / std::tan(fov * 0.5f), live.p00 = live.p11 / aspect;
}

// à la première présentation : étalonnage relu ; la carte d'ombres sera reconnue à sa description
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
    }
    std::fclose(f);
    if (b == 0 || !sign || !w)
        return;
    live.p00 = 0; // fixée à la première profondeur (rapport largeur / hauteur)
    live.A = float(a), live.B = float(b), g.depthReversed = b > 0, live.sunSign = float(sign);
    live.sunMapWidth = w, live.sunMapHeight = h, live.sunMapFormat = fmt;
    live.depthReady = live.calibrated = live.fromStore = true;
    live.framesSinceSun = 1000; // pas d'ombres avant la première mesure du soleil (quelques images)
    g_liveSunMapDesc[0] = w, g_liveSunMapDesc[1] = h, g_liveSunMapDesc[2] = fmt;
    Log("URTI direct : étalonnage stocké relu (profondeur = %.6f + %.6f / w, carte d'ombres %ux%u format %u) : ombres dès la première image",
        a, b, w, h, fmt);
}

// ------------------------------------------------------------------------------------------------
// étalonnage : direction du soleil (cartes d'ombres) et loi de profondeur, sur le processeur
// ------------------------------------------------------------------------------------------------

// moindres carrés S ≈ X M (X : n × 4, S : n × 3) par les équations normales ; faux si mal conditionné
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
    for (int col = 0; col < 4; col++) // Gauss avec pivot partiel
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

// À partir d'une capture lue (sommets de tous les flux, bornes des dessins) : soleil et profondeur.
void Calibrate(const uint8_t* so, const uint32_t* counters, const uint64_t* marks)
{
    const int mainSlot = g.captureSlot;
    if (mainSlot < 0)
        return;
    const float fov = 50.0f * 3.14159265f / 180.0f; // valeur arbitraire : les ombres n'en dépendent pas
    const float aspect = g.depthDesc.Height ? float(g.depthDesc.Width) / float(g.depthDesc.Height) : 16.0f / 9.0f;
    const float p11 = 1.0f / std::tan(fov * 0.5f), p00 = p11 / aspect;
    auto vertex = [&](int slot, uint64_t byteOffset) { return reinterpret_cast<const float*>(so + slot * kSlotBytes + byteOffset); };

    // loi de profondeur : z de découpage = A w + B, ajustée sur les sommets de l'écran
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
        Log("URTI direct : étalonnage impossible (%u sommets sur la profondeur de l'écran)", unsigned(n));
        return;
    }
    const double A = (n * swz - sw * sz) / (n * sww - sw * sw), B = (sz - A * sw) / n;
    if (!live.depthReady)
        Log("URTI direct : loi de profondeur = %.6f + %.6f / w (reflets possibles même sans soleil)", A, B);
    live.A = float(A), live.B = float(B), live.p00 = p00, live.p11 = p11;
    g.depthReversed = B > 0;
    live.depthReady = true;

    // soleil : pour chaque carte d'ombres, une direction par paire de dessins, puis vote
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
        std::unordered_map<uint64_t, uint32_t> rank; // rang du dessin parmi ceux de même signature
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
                // ajustement fidèle ? (sinon autre objet, ou animé)
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
                // d tel que A d = (0, 0, 1), A = M[0..2]ᵀ : la direction de la profondeur de la carte
                const double a[3][3] = { { M[0][0], M[1][0], M[2][0] }, { M[0][1], M[1][1], M[2][1] }, { M[0][2], M[1][2], M[2][2] } };
                const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                                   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
                if (std::fabs(det) < 1e-18)
                    continue;
                // troisième colonne de l'inverse : cofacteurs de la troisième ligne
                const double dx = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
                const double dy = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
                const double dz = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                // même rang dans les deux passes : très probablement le même objet (maillages répétés)
                dirs.push_back({ { -dx / len, -dy / len, -dz / len }, double(k) * (m == myRank ? 8.0 : 1.0) });
            }
        }
        // direction (au signe près) sur laquelle s'accordent le plus de sommets, à 2°
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
        // vue verticale (pluie, occlusion) : pas le soleil
        if (len <= 0 || std::fabs(sum[1] / len) > 0.995)
            continue;
        bestScore = slotBest;
        bestSlot = s;
        for (int q = 0; q < 3; q++)
            bestSun[q] = sum[q] / len;
    }
    if (bestScore <= 0)
    {
        // ciel couvert, nuit, intérieur : pas de soleil, pas d'ombres (après trois essais de suite)
        if (++live.missedCalibrations >= 3 && live.calibrated)
        {
            live.calibrated = false;
            Log("URTI direct : plus de carte d'ombres du soleil, ombres RT suspendues");
        }
        else
            Log("URTI direct : étalonnage sans carte d'ombres du soleil");
        return;
    }
    live.missedCalibrations = 0;
    g_liveSunDepth = g.slotDepth[bestSlot];
    live.sunDirty = true;
    live.framesSinceSun = 0;
    // les directions par paire ont un sens défini (profondeur décroissante de la carte) ; le soleil est
    // au-dessus de l'horizon : ce choix fixe la convention du jeu pour le suivi image par image
    live.sunSign = bestSun[1] < 0 ? -1.0f : 1.0f;
    if (bestSun[1] < 0)
        for (double& v : bestSun)
            v = -v;
    for (int q = 0; q < 3; q++)
        live.sun[q] = float(bestSun[q]);
    live.A = float(A), live.B = float(B), live.p00 = p00, live.p11 = p11;
    g.depthReversed = B > 0; // profondeur inversée : d = B / w, le ciel (w infini) à 0
    live.calibrated = true;
    live.verified = true;
    Log("URTI direct : soleil (%.3f, %.3f, %.3f), profondeur = %.6f + %.6f / w", bestSun[0], bestSun[1], bestSun[2], A, B);
    if (ID3D12Resource* map = reinterpret_cast<ID3D12Resource*>(g_liveSunDepth))
    {
        const D3D12_RESOURCE_DESC d = map->GetDesc();
        const bool changed = live.fromStore && (std::fabs(live.A - float(A)) > 1e-4f * std::max(1.0f, std::fabs(float(A))) ||
                                                std::fabs(live.B - float(B)) > 1e-3f * std::fabs(float(B)) || live.sunMapWidth != uint32_t(d.Width) ||
                                                live.sunMapHeight != d.Height || live.sunMapFormat != uint32_t(d.Format));
        live.sunMapWidth = uint32_t(d.Width), live.sunMapHeight = d.Height, live.sunMapFormat = uint32_t(d.Format);
        LiveSaveCalibration();
        if (changed)
            Log("URTI direct : l'étalonnage stocké ne correspondait plus, il est remplacé");
    }
}

// ------------------------------------------------------------------------------------------------
// GPU : structure d'accélération et ombres, avant les effets de ReShade
// ------------------------------------------------------------------------------------------------

bool LiveCreatePipelines(ID3D12Device* device)
{
    auto make = [&](const void* code, size_t size, int srvCount, ComPtr<ID3D12RootSignature>& rs, ComPtr<ID3D12PipelineState>& pso, uint32_t constants,
                    int uavCount = 1) {
        D3D12_ROOT_PARAMETER p[16] = {}; // SRV (t0…), UAV (u0…), constantes
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
    return make(g_live_convert, sizeof(g_live_convert), 3, live.convertRs, live.convertPso, 4) &&
           make(g_live_shadow, sizeof(g_live_shadow), 7, live.shadowRs, live.shadowPso, 24) &&
           make(g_live_mvcal, sizeof(g_live_mvcal), 4, live.mvcalRs, live.mvcalPso, 8) &&
           make(g_live_shadowtemporal, sizeof(g_live_shadowtemporal), 6, live.denoiseRs, live.denoisePso, 12) &&
           make(g_live_sunfit, sizeof(g_live_sunfit), 3, live.fitRs, live.fitPso, 4) &&
           make(g_live_sunvote, sizeof(g_live_sunvote), 1, live.voteRs, live.votePso, 4) &&
           make(g_live_composite, sizeof(g_live_composite), 6, live.compositeRs, live.compositePso, 12) &&
           make(g_live_uicompare, sizeof(g_live_uicompare), 5, live.uiRs, live.uiPso, 8) &&
           make(g_live_reflresolve, sizeof(g_live_reflresolve), 9, live.resolveRs, live.resolvePso, 20) &&
           make(g_live_reflect, sizeof(g_live_reflect), 8, live.reflectRs, live.reflectPso, 20, 2) &&
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
    if (r)
        live.retired[live.frame % 3].push_back(r);
    r.Reset();
}

// structure d'accélération assez grande pour `vertices` sommets
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
    live.blasAge = ~0u; // nouvelle structure : construction complète
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
        live.instances = MakeBuffer(device, sizeof(inst), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin = {};
        tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tin.NumDescs = 1;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ti = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &ti);
        live.tlas = UavBuffer(device, ti.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        live.tlasScratch = UavBuffer(device, ti.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Log("URTI direct : structure d'accélération pour %llu sommets (%.1f Mo)", (unsigned long long)cap, info.ResultDataMaxSizeInBytes / 1048576.0);
    return live.view && live.blas && live.blasScratch && live.tlas && live.instances;
}

// images (profondeur, ombres) à la taille de la profondeur du jeu
bool LiveEnsureImages(effect_runtime* runtime, ID3D12Device* device, const D3D12_RESOURCE_DESC& depthDesc)
{
    if (live.mask && live.width == depthDesc.Width && live.height == depthDesc.Height && live.depthFormat == depthDesc.Format)
        return true;
    Retire(live.depthBuf), Retire(live.shadowBuf), Retire(live.mask), Retire(live.prevShadowBuf);
    if (live.maskSrv.handle)
        runtime->get_device()->destroy_resource_view(live.maskSrv), live.maskSrv = {};
    live.width = uint32_t(depthDesc.Width), live.height = depthDesc.Height, live.depthFormat = depthDesc.Format;
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
    return live.depthBuf && live.shadowBuf;
}

// tampons du suivi du soleil (créés une fois)
bool LiveEnsureSun(ID3D12Device* device)
{
    if (live.sunBuf)
        return true;
    for (auto& r : live.pairsUpload)
        r = MakeBuffer(device, kLiveMaxPairs * 48ull, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    for (auto& r : live.sunReadback)
        r = MakeBuffer(device, 640, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
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

// toutes les 30 images : urti.ini modifié -> réglages relus ; shader recompilé dans urti_shaders -> pipeline
// recréé (l'ancien est libéré trois images plus tard)
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
            // valeurs par défaut à chaque relecture : une ligne retirée du fichier rend son réglage d'origine
            float combine = -1.0f, mask = 0.0f, debug = 0.0f, gameBias = 0.0015f, rayBias = 0.003f, sunRadius = 0.5f;
            float refl = 1.0f, channel = 1, invert = 0, smoothMin = 0.5f, f0 = 0.04f, debugReflect = 0, slot = 2, everywhere = 0, rays = 2, debugView = -1;
            float reflAlphaMin = 0.1f, reflAlphaMax = 0.5f, reflGamma = 1.0f, reflDepthTol = 0.1f, anchorAlpha = 0.0f, anchorGamma = 2.5f, alphaScreen = 1, blasRefit = 8;
            float vertexBudget = 4000000, probeBudget = 1000000, slotLimit = 0, uiPhoto = 1;
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
                value("anchor_alpha=", anchorAlpha), value("anchor_gamma=", anchorGamma); // anchor_alpha=0 : ancrage coupé
                value("alpha_screen=", alphaScreen), value("blas_refit=", blasRefit); // blas_refit=0 : toujours reconstruire
                // budget de géométrie (sommets ; 0 : sans budget), re-mesures par image, emplacement réduit (essai)
                value("vertex_budget=", vertexBudget), value("probe_budget=", probeBudget), value("slot_limit=", slotLimit), value("ui_photo=", uiPhoto);
            }
            std::fclose(f);
            live.iniCombine = int(combine); // -1 : le mode vient du menu de ReShade (UrtiMode)
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
            live.vertexBudget = uint64_t(std::max(0.0f, vertexBudget)), live.probeBudget = uint64_t(std::max(0.0f, probeBudget));
            live.slotLimit = uint64_t(std::max(0.0f, slotLimit));
            live.uiPhoto = uiPhoto != 0;
        }
        if (!first)
            Log("URTI direct : réglages relus (combine %d, masque %d, contrôle %u, game_bias %.4f, ray_bias %.4f, soleil %.2f° ; reflets %.2f, "
                "canal %u%s, lissé min %.2f, f0 %.3f, contrôle %u, cible %u)", live.iniCombine, g_liveShowMask, g_liveDebugGame, g_liveGameBias,
                g_liveRayBias, g_liveSunRadius, live.reflStrength, live.reflChannel, live.reflInvert ? " inversé" : "", live.smoothMin, live.f0,
                live.debugReflect, live.materialSlot);
    }
    struct Hot { const char* name; ID3D12RootSignature* rs; ComPtr<ID3D12PipelineState>* pso; };
    const Hot hot[3] = { { "live_shadow", live.shadowRs.Get(), std::addressof(live.shadowPso) },
                         { "live_composite", live.compositeRs.Get(), std::addressof(live.compositePso) },
                         { "live_reflect", live.reflectRs.Get(), std::addressof(live.reflectPso) } };
    for (int i = 0; i < 3; i++)
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
            Log("URTI direct : shader %s refusé (signature ou code invalide), l'ancien reste", hot[i].name);
            continue;
        }
        live.retiredPso[live.frame % 3].push_back(*hot[i].pso);
        *hot[i].pso = pso;
        Log("URTI direct : shader %s rechargé à chaud", hot[i].name);
    }
}

void LiveGbufferBarrier(command_list* cmd, uint32_t count, const resource* resources, const resource_usage* oldStates, const resource_usage* newStates)
{
    if (!g_liveCapture || !live.enabled || !OurDevice(cmd->get_device()))
        return;
    uint64_t target = 0, motionTarget = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (live.materialSlot < g_liveGbufferCount && live.reflStrength > 0)
            target = g_liveGbuffer[live.materialSlot];
        // vecteurs de mouvement : la cible du G-buffer en deux flottants 16 bits
        for (uint32_t k = 0; k < g_liveGbufferCount && !motionTarget; k++)
            if (g_liveGbuffer[k])
            {
                const DXGI_FORMAT f = reinterpret_cast<ID3D12Resource*>(g_liveGbuffer[k])->GetDesc().Format;
                if (f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_TYPELESS)
                    motionTarget = g_liveGbuffer[k];
            }
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
            Log("URTI direct : vecteurs de mouvement du jeu %llux%u copiés à chaque image (accumulation dans le temps)", (unsigned long long)d.Width, d.Height);
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
    if (!target)
        return;
    for (uint32_t i = 0; i < count; i++)
    {
        if (resources[i].handle != target)
            continue;
        const uint32_t before = PassD3D12State(static_cast<uint32_t>(oldStates[i])), after = PassD3D12State(static_cast<uint32_t>(newStates[i]));
        if (!(before & D3D12_RESOURCE_STATE_RENDER_TARGET) || (after & D3D12_RESOURCE_STATE_RENDER_TARGET))
            continue;
        ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(target);
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        if (PassTexelBytes(uint32_t(d.Format)) != 4 || d.SampleDesc.Count != 1)
            return;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!live.materialBuf || live.materialBytes < total)
        {
            Retire(live.materialBuf);
            live.materialBuf = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
            live.materialBytes = total;
            Log("URTI direct : matériau du G-buffer (cible %u) %llux%u format %d copié à chaque image", live.materialSlot, (unsigned long long)d.Width,
                d.Height, int(d.Format));
        }
        if (!live.materialBuf)
            return;
        ID3D12GraphicsCommandList* list = Native(cmd);
        Transition(list, res, D3D12_RESOURCE_STATES(after), D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = live.materialBuf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(after));
        live.materialPitch = fp.Footprint.RowPitch, live.materialWidth = uint32_t(d.Width), live.materialHeight = d.Height;
        live.materialFormat = uint32_t(d.Format), live.materialFrame = live.frame;
        return;
    }
}

// horodatage k (0 à 7) de l'image en cours
void LiveTime(ID3D12GraphicsCommandList* list, uint32_t k)
{
    if (live.timeHeap)
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (live.frame % 3) * 8 + k);
}

int LiveMainSlot()
{
    int best = -1;
    uint32_t most = 0;
    for (uint32_t s = 0; s < kSlots; s++)
        if (g.slotDepth[s] && g.screenLike[g.slotDepth[s]] == 1 && g.slotDraws[s] >= most)
            most = g.slotDraws[s], best = int(s);
    return best;
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
            Log("URTI : écran partagé %s", on ? "coupé" : "actif");
        }
    if (!live.enabled)
    {
        // ombres coupées : l'effet ne doit plus appliquer l'ancienne image d'ombres
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
    live.retired[live.frame % 3].clear();
    live.retiredPso[live.frame % 3].clear();
    if (!live.convertPso && !LiveCreatePipelines(g.device))
    {
        Log("URTI direct : shaders refusés, mode direct coupé");
        live.enabled = false;
        return;
    }
    LiveHotReload(g.device);
    if (live.iniCombine >= 0)
        g_liveCombine = live.iniCombine != 0;
    else if (const effect_uniform_variable mode = runtime->find_uniform_variable("urti.fx", "UrtiMode"); mode.handle)
    {
        int32_t m = 0;
        runtime->get_uniform_value_int(mode, &m, 1);
        g_liveCombine = m == 1;
        if (m != live.lastMode)
            Log("URTI direct : mode %s (menu de ReShade)", m == 1 ? "seulement les ombres que le jeu n'a pas" : "ombres RT complètes");
        live.lastMode = m;
    }

    // ce que le flux a reçu il y a trois images (lecture sans attente)
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
    std::vector<LiveRegion> regionsNow, probesNow;
    uint32_t cursorNow = 0, countersUsed = kSlots;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        marksNow.swap(g_liveMarks);
        if (slot >= 0)
            regionsNow.swap(g_liveRegions[slot]), cursorNow = g_liveCursor[slot];
        for (uint32_t s = 0; s < kSlots; s++)
            g_liveRegions[s].clear(), g_liveCursor[s] = 0;
        g_liveKeyRank.clear();
        if (slot >= 0)
            probesNow.swap(g_liveProbes[slot]);
        for (uint32_t s = 0; s < kSlots; s++)
            g_liveProbes[s].clear(), g_liveProbeUsed[s] = 0;
        live.seenNow.swap(g_liveSeen);
        g_liveSeen.clear();
        live.statOverflow += g_liveOverflow, live.statOutOfBudget += g_liveOutOfBudget, live.statReserved += g_liveReserved;
        g_liveOverflow = g_liveOutOfBudget = 0, g_liveReserved = 0;
        countersUsed = g_liveNextCounter;
        g_liveNextCounter = kSlots;
        if (!g_liveSunKeys.empty())
            g_livePrevSunKeys.swap(g_liveSunKeys);
        g_liveSunKeys.clear();
        g_liveSunMarked = 0;
    }
    // budget de géométrie : mesures arrivées (trois images plus tard), puis choix des objets de la scène RT
    // pour les images suivantes : par taille apparente décroissante jusqu'au budget (bonus aux objets déjà
    // dedans : pas de va-et-vient au bord du budget) ; les objets inconnus entrent d'office (mesurés ensuite)
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
                    live.objects[ids[i]] = { importance, live.frame, uint32_t(v[i * 4 + 2]) };
                }
                live.statsReadback[live.frame % 3]->Unmap(0, nullptr);
            }
            ids.clear();
        }
        if (live.objects.size() > 200000)
            live.objects.clear();
        std::unordered_set<uint32_t> known, admit, probe;
        if (live.vertexBudget > 0)
        {
            std::vector<std::pair<float, uint32_t>> ranked;
            uint64_t used = 0;
            for (const auto& [id, vertices] : live.seenNow)
            {
                const auto it = live.objects.find(id);
                if (it == live.objects.end())
                {
                    used += vertices; // inconnu : entre d'office
                    continue;
                }
                known.insert(id);
                ranked.push_back({ it->second.importance * (live.admitted.count(id) ? 1.3f : 1.0f), id });
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<std::pair<uint64_t, uint32_t>> stale;
            bool full = false;
            // un objet trop gros pour la place restante est sauté, les suivants peuvent entrer ; sommets
            // comptés : ceux réellement écrits (un ExecuteIndirect en réserve plus qu'il n'en dessine)
            for (const auto& [score, id] : ranked)
            {
                const uint32_t vertices = std::min(live.seenNow[id], std::max(live.objects[id].vertices, 3u));
                if (used + vertices <= live.vertexBudget)
                    used += vertices, admit.insert(id);
                else
                    full = true, stale.push_back({ live.objects[id].frame, id });
            }
            (void)full;
            // re-mesures : les mesures les plus anciennes d'abord
            std::sort(stale.begin(), stale.end());
            uint64_t probed = 0;
            for (const auto& [frame, id] : stale)
            {
                const uint32_t vertices = live.seenNow[id];
                if (probed + vertices > live.probeBudget)
                    continue;
                probed += vertices, probe.insert(id);
            }
        }
        if (live.frame % 300 == 150)
        {
            uint64_t unknownVerts = 0, knownVerts = 0;
            for (const auto& [id, vertices] : live.seenNow)
                (known.count(id) ? knownVerts : unknownVerts) += vertices;
            Log("URTI direct : budget : %zu objets à l'écran (%llu sommets mesurés, %llu pas encore mesurés), %zu dans la scène RT, %zu mesures en mémoire", live.seenNow.size(),
                (unsigned long long)knownVerts, (unsigned long long)unknownVerts, admit.size(), live.objects.size());
        }
        live.admitted = admit;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveKnown.swap(known), g_liveAdmit.swap(admit), g_liveProbeWant.swap(probe);
        g_liveBudget = live.vertexBudget > 0;
        g_liveSlotLimit = live.slotLimit;
        g_liveMainDepth = slot >= 0 ? g.slotDepth[slot] : 0;
        // tailles apprises des ExecuteIndirect (toutes les identités mesurées : la table reste petite)
        if (live.frame % 30 == 0)
        {
            g_liveLearned.clear();
            for (const auto& [id, o] : live.objects)
                if (o.vertices > 0 && live.frame - o.frame < 120)
                    g_liveLearned[id] = o.vertices;
        }
    }
    // bilan du suivi du soleil, lu trois images plus tard
    if (const float* r = nullptr; live.sunReadback[live.frame % 3] && SUCCEEDED(live.sunReadback[live.frame % 3]->Map(0, nullptr, (void**)&r)) && r)
    {
        if (live.calibrated)
        {
            live.sunFrames++, live.pairSum += uint64_t(r[4]);
            if (r[156] > 0) // rotation de la caméra mesurée (octet 624)
            {
                const double tr = r[144] + r[149] + r[154]; // trace de la matrice (lignes à 576, 592, 608)
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
                    live.forceCalibration = true; // vérification discrète de l'étalonnage stocké
                }
            }
            else
                live.framesSinceSun++;
            if (live.sunFrames == 300)
            {
                Log("URTI direct : soleil suivi (%.3f, %.3f, %.3f), mesuré sur %.0f %% des images, %.0f paires par image, %u cascades", r[0], r[1], r[2],
                    100.0 * live.sunAccepted / live.sunFrames, double(live.pairSum) / live.sunFrames, reinterpret_cast<const uint32_t*>(r)[8]);
                if (live.sunJitterCount)
                    Log("URTI direct : soleil d'une image à l'autre : %.3f° en moyenne, %.2f° au plus (caméra comprise)", live.sunJitterSum / live.sunJitterCount,
                        live.sunJitterMax);
                live.sunJitterSum = live.sunJitterMax = 0, live.sunJitterCount = 0;
                if (live.rotationValid)
                    Log("URTI direct : rotation de la caméra mesurée sur %u images sur 300 (cascades), %.3f° par image en moyenne", live.rotationValid,
                        live.rotationSum / live.rotationValid);
                live.rotationValid = 0, live.rotationSum = 0;
                live.sunFrames = live.sunAccepted = live.pairSum = 0;
            }
        }
        live.sunReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    if (live.mvConvention == 255 && live.mvErrorsReadback[live.frame % 3])
        if (const uint32_t* e = nullptr; SUCCEEDED(live.mvErrorsReadback[live.frame % 3]->Map(0, nullptr, (void**)&e)) && e)
        {
            const double meanMotion = e[33] ? e[32] / 1e6 / e[33] : 0;
            if (meanMotion > 0.0008) // la caméra bouge : les candidates se départagent
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
                        Log("URTI direct : convention des vecteurs de mouvement trouvée (%d : erreur %.4f, suivante %.4f) : accumulation des ombres active", best,
                            bestErr, secondErr);
                        if (live.verified || live.fromStore)
                            LiveSaveCalibration();
                    }
                    else
                        Log("URTI direct : vecteurs de mouvement pas encore départagés (meilleure %.4f, suivante %.4f), on continue", bestErr, secondErr);
                    std::fill(std::begin(live.mvErr), std::end(live.mvErr), 0.0), std::fill(std::begin(live.mvCount), std::end(live.mvCount), 0.0);
                    live.mvMovingFrames = 0;
                }
            }
            live.mvErrorsReadback[live.frame % 3]->Unmap(0, nullptr);
        }
    if (live.lastPresentDraws == 0 && live.bound)
    {
        live.statNoDraw++;
        // image sans dessin du jeu (image générée par DLSS) : l'image d'ombres précédente reste
        Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(g.counters.Get(), 0, g.zeros.Get(), 0, uint64_t(std::max(countersUsed, kSlots)) * 8ull);
        Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        for (uint32_t s = 0; s < kSlots; s++)
            g.slotDraws[s] = 0;
        live.frame++;
        return;
    }
    const effect_texture_variable var = runtime->find_texture_variable("urti.fx", "UrtiDepth");
    resource_view depthSrv = {}, depthSrgb = {};
    if (var.handle)
        runtime->get_texture_binding(var, &depthSrv, &depthSrgb);
    ID3D12Resource* depthCopy = depthSrv.handle ? reinterpret_cast<ID3D12Resource*>(runtime->get_device()->get_resource_from_view(depthSrv).handle) : nullptr;
    const effect_uniform_variable active = runtime->find_uniform_variable("urti.fx", "UrtiActive");
    if (depthCopy && !live.depthReady)
    {
        // la loi de profondeur est mesurée à chaque image sur le GPU : seule la projection est supposée
        LiveSetProjection(depthCopy->GetDesc());
        live.depthReady = true;
    }
    if (slot < 0 || !depthCopy || !live.depthReady)
    {
        if (active.handle)
            runtime->set_uniform_value_bool(active, false);
        live.frame++;
        return;
    }
    const D3D12_RESOURCE_DESC depthDesc = depthCopy->GetDesc();
    if (live.fromStore && live.p00 == 0)
        LiveSetProjection(depthDesc);
    live.lastFilled = cursorNow / 16;
    if (!LiveEnsureCapacity(device.Get(), std::max<uint64_t>(live.lastFilled, 300000)) || !LiveEnsureImages(runtime, g.device, depthDesc) ||
        !LiveEnsureSun(g.device))
        return;

    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!live.timeHeap)
    {
        D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 24, 0 };
        g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&live.timeHeap));
        for (auto& r : live.timeReadback)
            r = MakeBuffer(g.device, 8 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    // minutage lu trois images plus tard : structure, soleil, ombres, reflets, composition (ms)
    if (const uint64_t* t = nullptr; live.timeReadback[live.frame % 3] && SUCCEEDED(live.timeReadback[live.frame % 3]->Map(0, nullptr, (void**)&t)) && t)
    {
        UINT64 freq = 0;
        if (ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()); q && SUCCEEDED(q->GetTimestampFrequency(&freq)) && freq &&
            t[5] > t[0] && t[5] - t[0] < freq)
        {
            for (int k = 0; k < 5; k++)
                live.timeSum[k] += double(t[k + 1] - t[k]) * 1000.0 / double(freq);
            if (t[7] > t[6] && t[7] - t[6] < freq)
                live.timeSum[5] += double(t[7] - t[6]) * 1000.0 / double(freq);
            if (++live.timeFrames == 300)
            {
                const double n = live.timeFrames;
                Log("URTI direct : GPU par image : structure %.2f ms, soleil et profondeur %.2f, copie carte du jeu %.2f, ombres (%u rayons) %.2f, "
                    "reflets %.2f, composition %.2f", live.timeSum[0] / n, live.timeSum[1] / n, live.timeSum[2] / n, live.shadowRays, live.timeSum[3] / n,
                    live.timeSum[4] / n, live.timeSum[5] / n);
                const uint32_t builds = live.statRebuilds + live.statUpdates;
                if (builds)
                    Log("URTI direct : structure d'accélération : %.0f sommets en moyenne (capacité %llu), %u reconstructions et %u mises à jour",
                        double(live.statVertices) / builds, (unsigned long long)live.capacity, live.statRebuilds, live.statUpdates);
                live.statRebuilds = live.statUpdates = 0, live.statVertices = 0;
                if (live.statBudgetFrames)
                {
                    const double f = double(live.statBudgetFrames);
                    Log("URTI direct : budget de géométrie %llu sommets : %.0f dessins dans la scène RT (%.0f sommets réservés), %.0f hors budget, "
                        "%.0f re-mesurés, %.1f refusés (emplacement plein) par image", (unsigned long long)live.vertexBudget, live.statDraws / f,
                        live.statReserved / f, live.statOutOfBudget / f, live.statProbes / f, live.statOverflow / f);
                }
                live.statDraws = live.statOutOfBudget = live.statProbes = live.statOverflow = live.statBudgetFrames = live.statReserved = 0;
                std::fill(std::begin(live.timeSum), std::end(live.timeSum), 0.0);
                live.timeFrames = 0;
            }
        }
        live.timeReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    LiveTime(list.Get(), 0);
    // 1. sommets de l'écran vers l'espace caméra (NaN au-delà de ce qui a été rempli dans chaque zone) ;
    // seulement la partie du flux attribuée à cette image : la structure ne couvre qu'elle
    Transition(list.Get(), g.so.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, srv);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, srv);
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
        return;
    // par zone : identité, début de la zone du même objet dans l'image précédente (+ 1 ; 0 : inconnue)
    ID3D12Resource* linksBuf = nullptr;
    {
        live.pendingStart.clear();
        std::vector<uint32_t> links(std::max<size_t>(regionsNow.size(), 1) * 2, 0);
        for (size_t i = 0; i < regionsNow.size(); i++)
        {
            const LiveRegion& r = regionsNow[i];
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
    if (!linksBuf)
        return;
    list->SetComputeRootSignature(live.convertRs.Get());
    list->SetPipelineState(live.convertPso.Get());
    list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
    list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, regionsBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, live.view->GetGPUVirtualAddress());
    const uint32_t converted = std::max(3u, uint32_t(std::min<uint64_t>(cursorNow / 16, live.capacity)) / 3 * 3);
    const struct { float p00, p11; uint32_t count, regionCount; } cc = { live.p00, live.p11, converted, uint32_t(regionsNow.size()) };
    list->SetComputeRoot32BitConstants(4, 4, &cc, 0);
    list->Dispatch(UINT((converted + 255) / 256), 1, 1);
    // 1a. mesure de chaque objet capté, de la scène RT et de la zone de mesure (budget de géométrie)
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
            std::memcpy(p, regionsNow.data(), regionsNow.size() * sizeof(LiveRegion));
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
            ids.clear();
            for (const LiveRegion& r : regionsNow)
                ids.push_back(r.id);
            for (const LiveRegion& r : probesNow)
                ids.push_back(r.id);
        }
        live.statDraws += regionsNow.size(), live.statProbes += probesNow.size(), live.statBudgetFrames++;
    }
    Transition(list.Get(), live.view.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);

    LiveTime(list.Get(), 1);
    // 1b. soleil de cette image : paires écran / carte d'ombres de même signature, ajustées puis votées
    if (live.sunDirty)
    {
        void* p = nullptr;
        if (SUCCEEDED(live.sunUpload->Map(0, nullptr, &p)))
        {
            const float s[12] = { live.sun[0], live.sun[1], live.sun[2], 1, 0, 0, 0, 0, 0, live.A, live.B, live.B != 0 ? 1.0f : 0.0f };
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
        uint32_t camBase, mapBase, camCounter, mapCounter; // débuts dans le flux (octets), compteurs
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
            // même rang dans les deux passes : très probablement le même objet (poids 8)
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
            const struct { uint32_t count; float cosTol, minShare, sunSign; } vc = { uint32_t(pairs.size()), std::cos(2.0f * 3.14159265f / 180.0f), 0.35f, live.sunSign };
            list->SetComputeRoot32BitConstants(2, 4, &vc, 0);
            list->Dispatch(1, 1, 1);
            Transition(list.Get(), live.pairDirs.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            // aucune paire : bilan « non mesuré », le soleil précédent reste, plus de cascade connue
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.sunBuf.Get(), 16, g.zeros.Get(), 0, 20); // pas la loi de profondeur (36 à 47)
            list->CopyBufferRegion(live.sunBuf.Get(), 624, g.zeros.Get(), 0, 32); // rotation non mesurée, plus de cascades gardées
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    // 1c. loi de profondeur de cette image, mesurée sur les sommets de l'écran
    {
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = live.sunBuf.Get();
        list->ResourceBarrier(1, &ub);
        list->SetComputeRootSignature(live.depthfitRs.Get());
        list->SetPipelineState(live.depthfitPso.Get());
        list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
        list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, regionsBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3, live.sunBuf->GetGPUVirtualAddress());
        const uint32_t dc[4] = { uint32_t(regionsNow.size()), cursorNow, 0, 0 };
        list->SetComputeRoot32BitConstants(4, 4, dc, 0);
        list->Dispatch(1, 1, 1);
    }
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);

    // 2. structures d'accélération
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer = { live.view->GetGPUVirtualAddress(), 12 };
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = converted;
    // même liste de dessins (zones, tailles, identités) qu'à la dernière construction : les triangles sont
    // les mêmes, seules leurs positions ont changé (caméra, vent, objets mobiles) : mise à jour
    uint64_t layout = 1469598103934665603ull ^ converted;
    for (const LiveRegion& r : regionsNow)
        layout = (layout ^ (uint64_t(r.offset) << 32 ^ r.capacity ^ uint64_t(r.id) << 17)) * 1099511628211ull;
    const bool refit = live.blasRefit > 0 && live.blasAge < live.blasRefit && layout == live.blasLayout;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                      (refit ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(0));
    if (refit)
        bd.SourceAccelerationStructureData = live.blas->GetGPUVirtualAddress(), live.blasAge++, live.statUpdates++;
    else
        live.blasLayout = layout, live.blasAge = 0, live.statRebuilds++;
    live.statVertices += converted;
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.NumDescs = 1;
    bd.Inputs.pGeometryDescs = &geom;
    bd.DestAccelerationStructureData = live.blas->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = live.blasScratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = live.blas.Get();
    list->ResourceBarrier(1, &uav);
    {
        D3D12_RAYTRACING_INSTANCE_DESC inst = {};
        inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
        inst.InstanceMask = 0xFF;
        inst.AccelerationStructure = live.blas->GetGPUVirtualAddress();
        void* p = nullptr;
        if (SUCCEEDED(live.instances->Map(0, nullptr, &p)))
        {
            std::memcpy(p, &inst, sizeof(inst));
            live.instances->Unmap(0, nullptr);
        }
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    td.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    td.Inputs.NumDescs = 1;
    td.Inputs.InstanceDescs = live.instances->GetGPUVirtualAddress();
    td.DestAccelerationStructureData = live.tlas->GetGPUVirtualAddress();
    td.ScratchAccelerationStructureData = live.tlasScratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    uav.UAV.pResource = live.tlas.Get();
    list->ResourceBarrier(1, &uav);

    // 3a. profondeur de l'image précédente gardée (accumulation dans le temps)
    if (live.depthPrevBuf)
    {
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(live.depthPrevBuf.Get(), live.depthBuf.Get());
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    // 3. profondeur du jeu (copie de ReShade) vers un tampon lisible par le shader
    const D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(list.Get(), depthCopy, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
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
    }
    Transition(list.Get(), depthCopy, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);
    Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);

    LiveTime(list.Get(), 2);
    // 3b. carte d'ombres du jeu (son ombre est recalculée pixel par pixel : nos ombres ne s'ajoutent
    // que là où le jeu n'en met pas)
    bool atlasValid = false;
    // (lue aussi pour reconnaître les trous du feuillage découpé : la carte est la vue du soleil)
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
            Log("URTI direct : carte d'ombres du jeu %llux%u format %d recopiée à chaque image (%.0f Mo)", (unsigned long long)ad.Width, ad.Height,
                int(ad.Format), atotal / 1048576.0);
        }
        if (known && live.atlasBuf)
        {
            live.atlasPitch = afp.Footprint.RowPitch;
            live.atlasD24 = ad.Format == DXGI_FORMAT_R24G8_TYPELESS || ad.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
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
        }
    }

    LiveTime(list.Get(), 3);
    // masque de l'image précédente gardé (images générées, mode mélange)
    if (live.prevShadowBuf)
    {
        Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.prevShadowBuf.Get(), 0, live.shadowBuf.Get(), 0, uint64_t(live.shadowPitch) * live.height);
        Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // 4. un rayon par pixel vers le soleil
    list->SetComputeRootSignature(live.shadowRs.Get());
    list->SetPipelineState(live.shadowPso.Get());
    list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, live.sunBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, atlasValid ? live.atlasBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    const bool motionOk = live.mvBuf && live.mvFrame != ~0ull && live.frame <= live.mvFrame + 1 && live.mvWidth == live.width &&
                          live.mvHeight == live.height && live.depthPrevBuf;
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
    list->SetComputeRootUnorderedAccessView(7, live.shadowRawBuf ? live.shadowRawBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    const bool d24 = depthDesc.Format == DXGI_FORMAT_R24G8_TYPELESS || depthDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    // soleil mesuré dans les 30 dernières images (3 de retard de lecture) : sinon pas d'ombres RT
    const bool sunNow = live.calibrated && live.framesSinceSun < 30;
    const uint32_t flags = (atlasValid ? 1u : 0u) | (live.atlasD24 ? 2u : 0u) | (g_liveCombine ? 4u : 0u) | (g_liveDebugGame ? 8u : 0u) |
                           (sunNow ? 0u : 16u) | (live.shadowRays << 8) | (live.alphaScreen ? 2048u : 0u);
    const struct
    {
        float sunSign, gameBias, unused, bias;
        uint32_t width, height, depthPitch, shadowPitch;
        float A, B, p00, p11, clearDepth;
        uint32_t d24, atlasPitch, flags;
        uint32_t motionPitch, motionConvention;
        float temporalAlpha;
        uint32_t debugMode, frameIndex, pad4, pad5, pad6;
    } sc = { live.sunSign, g_liveGameBias, g_liveSunRadius * 3.14159265f / 180.0f, g_liveRayBias, live.width, live.height, live.depthPitch, live.shadowPitch,
             live.A, live.B, live.p00, live.p11, g.depthReversed ? 0.0f : 1.0f, d24 ? 1u : 0u, live.atlasPitch, flags,
             live.mvPitch, live.shadowRawBuf ? 255u : (motionOk && live.prevShadowBuf ? live.mvConvention : 255u), live.temporalAlpha,
             0u, uint32_t(live.frame), 0, 0, 0 };
    list->SetComputeRoot32BitConstants(8, 24, &sc, 0);
    list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
    // débruitage : flou guidé par la profondeur, historique reprojeté, bornage statistique, mélange adaptatif
    if (live.shadowRawBuf && live.denoisePso)
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
        list->SetComputeRootUnorderedAccessView(6, live.shadowBuf->GetGPUVirtualAddress());
        const struct
        {
            uint32_t width, height, depthPitch, shadowPitch, motionPitch, motionConvention, depth24, debugMode;
            float alphaMin, alphaMax, gamma;
            uint32_t pad;
        } dn = { live.width, live.height, live.depthPitch, live.shadowPitch, live.mvPitch,
                 motionOk && live.prevShadowBuf ? live.mvConvention : 255u, d24 ? 1u : 0u, live.debugView >= 1 && live.debugView <= 3 ? live.debugView : 0u,
                 0.1f, 0.5f, 1.5f, 0 };
        list->SetComputeRoot32BitConstants(7, 12, &dn, 0);
        list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
        Transition(list.Get(), live.shadowRawBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // recherche de la convention des vecteurs de mouvement (tant qu'elle n'est pas trouvée)
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
            list->Dispatch(16, 1, 1); // 4096 échantillons
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
    list->CopyBufferRegion(live.sunReadback[live.frame % 3].Get(), 0, live.sunBuf.Get(), 0, 640);
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    LiveTime(list.Get(), 4);
    // réglages des reflets dans le menu de ReShade : force, sol mouillé (urti.ini garde la main s'il impose
    // un lissé négatif, c'est-à-dire un sol mouillé)
    float menuReflect = 1.0f, menuWet = 0.0f;
    {
        int32_t view = live.iniDebugView;
        if (view < 0)
        {
            view = 0;
            if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiDebug"); v.handle)
                runtime->get_uniform_value_int(v, &view, 1);
        }
        if (uint32_t(view) != live.debugView)
            Log("URTI direct : vue de diagnostic %d", view);
        live.debugView = uint32_t(std::clamp(view, 0, 9));
    }
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiGenerated"); v.handle)
    {
        int32_t m = 0;
        runtime->get_uniform_value_int(v, &m, 1);
        live.fgMode = uint32_t(std::clamp(m, 0, 2));
    }
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiReflect"); v.handle)
        runtime->get_uniform_value_float(v, &menuReflect, 1);
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiWet"); v.handle)
        runtime->get_uniform_value_float(v, &menuWet, 1);
    live.reflEffective = live.reflStrength * menuReflect;
    const float smoothMinEffective = live.smoothMin < 0 ? live.smoothMin : (menuWet > 0.01f ? -menuWet : live.smoothMin);
    // 4b. reflets : un rayon réfléchi par pixel lisse (matériau du G-buffer copié dans cette image)
    if (live.reflEffective > 0 || live.debugReflect)
    {
        const uint64_t bytes = uint64_t(live.width) * live.height * 16;
        if (!live.reflectBuf || live.reflectBytes < bytes)
        {
            Retire(live.reflectBuf);
            live.reflectBuf = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.reflectBytes = bytes;
        }
        const bool material = live.materialBuf && live.materialFrame != ~0ull && live.frame <= live.materialFrame + 2 &&
                              live.materialWidth == live.width && live.materialHeight == live.height;
        if (live.anchorBytes < bytes)
        {
            for (auto& a : live.anchor)
                Retire(a), a = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.anchorBytes = bytes;
            live.anchorOut = -1;
        }
        if (live.reflectBuf && live.reflectPso && live.anchor[0] && live.anchor[1])
        {
            // ancrage de cette image : l'autre tampon garde celui de la précédente
            live.anchorOut = live.anchorOut < 0 ? 0 : 1 - live.anchorOut;
            live.anchorPrevOk = live.anchorFrame != ~0ull && live.anchorFrame + 1 == live.frame;
            live.anchorFrame = live.frame;
            if (live.materialBuf)
                Transition(list.Get(), live.materialBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.reflectRs.Get());
            list->SetPipelineState(live.reflectPso.Get());
            list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, live.materialBuf ? live.materialBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(4, regionsBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(5, linksBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(6, live.view->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(7, live.viewPrev ? live.viewPrev->GetGPUVirtualAddress() : live.view->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(8, live.reflectBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(9, live.anchor[live.anchorOut]->GetGPUVirtualAddress());
            const struct
            {
                uint32_t width, height, depthPitch, materialPitch;
                float A, B, p00, p11, clearDepth;
                uint32_t d24, channel;
                float smoothMin, bias;
                uint32_t flags;
                float f0;
                uint32_t reflectPitch, regionCount, anchors, prevVertexCount, pad1;
            } rc = { live.width, live.height, live.depthPitch, live.materialPitch, live.A, live.B, live.p00, live.p11, g.depthReversed ? 0.0f : 1.0f,
                     d24 ? 1u : 0u, live.reflChannel, smoothMinEffective, 0.002f, (material ? 1u : 0u) | (live.reflInvert ? 2u : 0u) | (live.reflEverywhere ? 4u : 0u) | (live.debugView == 7 ? 8u : 0u) |
                     (live.debugView == 4 || live.debugReflect == 1 ? 16u : 0u) | (live.alphaScreen ? 32u : 0u), live.f0, live.width * 16,
                     uint32_t(regionsNow.size()), live.viewPrev && live.prevValid ? 1u : 0u, uint32_t(live.capacity), 0 };
            list->SetComputeRoot32BitConstants(10, 20, &rc, 0);
            list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
            Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (live.materialBuf)
                Transition(list.Get(), live.materialBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER uavb = {};
            uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavb.UAV.pResource = live.reflectBuf.Get();
            list->ResourceBarrier(1, &uavb);
        }
    }

    LiveTime(list.Get(), 5);
    if (live.timeHeap && live.timeReadback[live.frame % 3])
        list->ResolveQueryData(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (live.frame % 3) * 8, 6, live.timeReadback[live.frame % 3].Get(), 0);
    // 5. image d'ombres pour l'effet
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
    // sommets de cette image gardés pour la suivante (mémoire géométrique)
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
    Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);

    // 6. compteurs : lus plus tard, puis remis à zéro pour l'image suivante
    Transition(list.Get(), g.counters.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(live.counterReadback[live.frame % 3].Get(), 0, g.counters.Get(), 0, kSlots * 8ull);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(g.counters.Get(), 0, g.zeros.Get(), 0, uint64_t(std::max(countersUsed, kSlots)) * 8ull);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
    for (uint32_t s = 0; s < kSlots; s++)
        g.slotDraws[s] = 0;

    live.maskFrame = live.frame;
    if (active.handle)
        runtime->set_uniform_value_bool(active, true);
    const effect_uniform_variable showMask = runtime->find_uniform_variable("urti.fx", "UrtiShowMask");
    if (showMask.handle && g_liveShowMask)
        runtime->set_uniform_value_bool(showMask, true);
    if (!live.bound && active.handle)
    {
        live.bound = true;
        live.boundFrame = live.frame;
        Log("URTI direct : première image : flux %d, %u dessins, %llu sommets remplis (structure %llu), profondeur %ux%u format %d, uniforme %s",
            slot, drawsThisFrame, (unsigned long long)live.lastFilled, (unsigned long long)live.capacity, live.width, live.height,
            (int)depthDesc.Format, active.handle ? "trouvé" : "ABSENT");
    }
    live.frame++;
}

// ombres RT posées sur l'image présentée, sur la liste immédiate de ReShade (exécutée avant la
// présentation). Toutes les présentations passent ici, y compris les images générées par la génération
// d'images (DLSS-G / FSR), qui sont fabriquées à partir de l'image du jeu avant ReShade : sans cela,
// une image sur deux n'aurait pas les ombres RT.
void LiveComposite(effect_runtime* runtime)
{
    if (!live.enabled || !live.depthReady || !live.bound || !live.shadowBuf || !live.compositePso)
        return;
    // masque d'une image trop ancienne (calcul sauté : étalonnage, profondeur absente…) : rien plutôt
    // que des ombres figées qui ne suivent plus la caméra
    if (live.maskFrame == ~0ull || live.frame > live.maskFrame + 2)
        return;
    if (live.overlayOpen)
        return; // menu de ReShade ouvert : il doit rester lisible (la passe le recouvrirait)
    if (live.fgMode == 1 && live.lastPresentDraws == 0)
        return; // image générée laissée telle quelle (réglage du menu)
    const effect_technique technique = runtime->find_technique("urti.fx", "URTI");
    const bool techniqueOn = runtime->get_effects_state() && (!technique.handle || runtime->get_technique_state(technique));
    if (techniqueOn != live.lastTechnique)
        Log("URTI direct : %s dans ReShade", techniqueOn ? "effet URTI actif" : "effet URTI coupé (ou effets coupés) : pas d'ombres RT");
    live.lastTechnique = techniqueOn;
    if (!techniqueOn)
        return;
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
    // photos de l'écran avant l'interface : tampons (même disposition que imageIn), images d'écran connues,
    // choix d'après les comparaisons lues trois présentations plus tard
    if (live.snapBytes < total)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t k = 0; k < kLiveSnaps; k++)
        {
            Retire(live.snaps[k]);
            live.snaps[k] = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
            g_liveSnap[k] = live.snaps[k].Get();
        }
        g_liveSnapFp = fp;
        live.snapBytes = total;
        live.uiChoice = -1;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
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
        // la première photo presque identique à l'image finale : celle d'avant l'interface
        int choice = -1;
        float best = 0;
        for (uint32_t k = 0; k < live.uiCountAt[uiSlot] && c[4]; k++)
        {
            const float same = float(c[k]) / float(c[4]);
            if (same >= 0.6f)
            {
                choice = int(k), best = same;
                break;
            }
        }
        if (choice != live.uiChoice)
            Log(choice >= 0 ? "URTI direct : image avant l'interface du jeu : photo %d (%.1f %% des pixels identiques à l'image finale)"
                            : "URTI direct : pas de photo avant l'interface du jeu (%d, %.1f)", choice, best * 100.0f);
        live.uiChoice = choice;
        live.uiReadback[uiSlot]->Unmap(0, nullptr);
    }
    uint32_t snapCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snapCount = std::min(g_liveSnapCount, kLiveSnaps);
    }
    // réglages de l'effet (menu de ReShade) : force, écran partagé, masque seul
    float strength = 0.55f;
    bool split = false, showMask = false;
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiStrength"); v.handle)
        runtime->get_uniform_value_float(v, &strength, 1);
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiSplit"); v.handle)
        runtime->get_uniform_value_bool(v, &split, 1);
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiShowMask"); v.handle)
        runtime->get_uniform_value_bool(v, &showMask, 1);
    showMask = showMask || g_liveShowMask || (live.debugView >= 1 && live.debugView <= 3);
    if (strength != live.lastStrength || split != live.lastSplit || showMask != live.lastShowMask)
        Log("URTI direct : réglages du menu : force %.2f, écran partagé %d, masque seul %d", strength, split, showMask);
    live.lastStrength = strength, live.lastSplit = split, live.lastShowMask = showMask;

    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_TEXTURE_COPY_LOCATION tex = {}, buf = {};
    tex.pResource = bb;
    tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buf.PlacedFootprint = fp;
    const uint32_t slotTime = uint32_t((live.frame + 2) % 3); // image dont les effets viennent d'être enregistrés
    if (live.timeHeap)
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 6);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    buf.pResource = live.imageIn.Get();
    list->CopyTextureRegion(&buf, 0, 0, 0, &tex, nullptr);
    Transition(list.Get(), live.imageIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    for (uint32_t k = 0; k < snapCount; k++)
        Transition(list.Get(), live.snaps[k].Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    // comparaison des photos de cette image à l'image finale (lue trois présentations plus tard)
    live.uiCountAt[uiSlot] = 0;
    if (snapCount && live.uiPso)
    {
        list->CopyBufferRegion(live.uiCounts.Get(), 0, g.zeros.Get(), 0, 32);
        D3D12_RESOURCE_BARRIER cb = {};
        cb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        cb.Transition.pResource = live.uiCounts.Get();
        cb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // copie implicite (tampon) : état gardé
        list->SetComputeRootSignature(live.uiRs.Get());
        list->SetPipelineState(live.uiPso.Get());
        list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
        for (uint32_t k = 0; k < kLiveSnaps; k++)
            list->SetComputeRootShaderResourceView(1 + k, live.snaps[std::min(k, snapCount - 1)]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(5, live.uiCounts->GetGPUVirtualAddress());
        const uint32_t uc[8] = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format == 3 ? 8u : 4u, snapCount, 0, 0, 0 };
        list->SetComputeRoot32BitConstants(6, 8, uc, 0);
        list->Dispatch((uint32_t(bd.Width) / 4 + 7) / 8, (bd.Height / 4 + 7) / 8, 1);
        Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.uiReadback[uiSlot].Get(), 0, live.uiCounts.Get(), 0, 32);
        Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.uiCountAt[uiSlot] = snapCount;
        (void)cb;
    }
    live.uiFrame++;
    // photo d'avant l'interface de cette image, si le choix est sûr et qu'elle a été prise
    ID3D12Resource* preUi = live.uiPhoto && live.uiChoice >= 0 && uint32_t(live.uiChoice) < snapCount ? live.snaps[live.uiChoice].Get() : nullptr;
    Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    // vues 4 à 8 : lissé, poids, couleur reflétée, distance de touche (dans le canal du lissé), confiance
    const uint32_t reflectView = live.debugView == 4 ? 1u : live.debugView == 5 ? 2u : live.debugView == 6 ? 3u : live.debugView == 7 ? 1u :
                                 live.debugView == 8 ? 3u : live.debugView == 9 ? 4u : live.debugReflect;
    const bool reflections = live.reflectBuf && (live.reflEffective > 0 || reflectView);
    if (reflections)
        Transition(list.Get(), live.reflectBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    // reflets stabilisés dans le temps (image réelle : mise à jour de l'historique ; image générée : le
    // dernier résultat est repris tel quel)
    const bool isGenerated = live.lastPresentDraws == 0;
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
        if (live.reflHist[0] && live.reflHist[1])
        {
            if (isGenerated && live.reflHistLast >= 0)
                reflHistOut = live.reflHistLast;
            else if (!isGenerated)
            {
                const int prev = live.reflHistLast, cur = prev == 0 ? 1 : 0;
                const bool motionOk = live.mvBuf && live.mvFrame != ~0ull && live.frame <= live.mvFrame + 2 && live.mvWidth == live.width &&
                                      live.mvHeight == live.height && live.depthPrevBuf && prev >= 0;
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
                // ancrage (point reflété retrouvé à l'image précédente, identités) : celui de cette image et le précédent
                const bool anchored = live.anchorOut >= 0 && live.anchor[0] && live.anchor[1] && live.anchorFrame + 1 == live.frame; // écrit par les effets de cette image
                if (anchored)
                    for (auto& a : live.anchor)
                        Transition(list.Get(), a.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                list->SetComputeRootShaderResourceView(7, anchored ? live.anchor[live.anchorOut]->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(8, anchored ? live.anchor[1 - live.anchorOut]->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(9, live.reflHist[cur]->GetGPUVirtualAddress());
                const bool d24r = live.depthFormat == DXGI_FORMAT_R24G8_TYPELESS || live.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
                const struct
                {
                    uint32_t width, height, pitch, format, renderWidth, renderHeight, motionPitch, motionConvention, depthPitch, depth24;
                    float alpha, maxLuminance;
                    uint32_t flags;
                    float p00, p11;
                    float alphaMax, gamma, depthTol, anchorAlpha, anchorGamma;
                } rc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.mvPitch,
                         motionOk ? live.mvConvention : 255u, live.depthPitch, d24r ? 1u : 0u, live.reflAlphaMin, 4.0f,
                         (motionOk ? 1u : 0u) | (live.debugView == 8 ? 4u : 0u) | (anchored && live.anchorPrevOk ? 8u : 0u), live.p00, live.p11,
                         live.reflAlphaMax, live.reflGamma, live.reflDepthTol, live.anchorAlpha, live.anchorGamma };
                list->SetComputeRoot32BitConstants(10, 20, &rc, 0);
                list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
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
    }
    list->SetComputeRootSignature(live.compositeRs.Get());
    list->SetPipelineState(live.compositePso.Get());
    list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, reflections ? live.reflectBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    const bool generated = live.lastPresentDraws == 0;
    const bool blend = generated && live.fgMode == 2 && live.prevShadowBuf;
    if (blend)
        Transition(list.Get(), live.prevShadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(3, blend ? live.prevShadowBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(4, reflHistOut >= 0 ? live.reflHist[reflHistOut]->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(5, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(6, live.imageOut->GetGPUVirtualAddress());
    const struct
    {
        uint32_t width, height, pitch, format, maskWidth, maskHeight, maskPitch;
        float strength;
        uint32_t split, showMask;
        float reflStrength;
        uint32_t debugReflect;
    } cc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.shadowPitch, strength,
             split ? 1u : 0u, showMask ? 1u : 0u, (reflections && !(generated && live.fgMode == 2)) ? std::max(live.reflEffective, reflectView ? 1.0f : 0.0f) : 0.0f,
             (reflections ? reflectView : 0u) | (blend ? 0x100u : 0u) | (reflHistOut >= 0 ? 0x200u : 0u) | (preUi ? 0x400u : 0u) };
    list->SetComputeRoot32BitConstants(7, 12, &cc, 0);
    list->Dispatch((cc.width + 7) / 8, (cc.height + 7) / 8, 1);
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
    Transition(list.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    buf.pResource = live.imageOut.Get();
    list->CopyTextureRegion(&tex, 0, 0, 0, &buf, nullptr);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Transition(list.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (live.timeHeap && live.timeReadback[slotTime])
    {
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 7);
        list->ResolveQueryData(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 6, 2, live.timeReadback[slotTime].Get(), 6 * 8);
    }
    live.statComposited++;
}

bool OnOpenOverlay(effect_runtime*, bool open, input_source)
{
    live.overlayOpen = open;
    return false;
}

// photo de l'écran après les effets (fichier déclencheur urti_photo, ou présentations listées dans urti.ini
// photo_frames= : urti_photo_<présentation>.ppm, pour comparer deux lancements image par image)
void OnReshadePresent(effect_runtime* runtime)
{
    if (!OurDevice(runtime->get_device()))
        return;
    LiveComposite(runtime);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveSnapCount = 0; // photos de la prochaine image
    }
    if (++live.statPresents == 300)
    {
        Log("URTI direct : sur 300 présentations, %u sans dessin du jeu, %u passages dans les effets, %u avec ombres RT appliquées",
            live.statNoDraw, live.statEffects, live.statComposited);
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
    // pas avant que ReShade ait fini de charger ses effets (quelques dixièmes de seconde)
    if (g.frame < 300 || g.frame % 15 != 0 || GetFileAttributesA(trigger.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    if (live.enabled && (!live.bound || live.frame < live.boundFrame + 60))
        return; // ombres pas encore prêtes : la photo attend
    DeleteFileA(trigger.c_str());
    // lecture directe du tampon d'affichage (gère le HDR10 du jeu, que capture_screenshot rend brut)
    const resource back = runtime->get_current_back_buffer();
    if (!back.handle)
        return;
    const std::string path = g_dir + "\\urti_photo" + std::to_string(live.photoIndex++) + ".ppm";
    runtime->get_command_queue()->flush_immediate_command_list(); // les effets de cette image d'abord
    SaveBackBuffer(reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()), reinterpret_cast<ID3D12Resource*>(back.handle), path, 1);
    Log("URTI : photo %s (ombres RT %s)", path.c_str(), live.enabled ? "actives" : "coupées");
}

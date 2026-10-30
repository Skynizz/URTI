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
    uint64_t capacity = 0; // sommets de la structure
    uint32_t width = 0, height = 0, depthPitch = 0, shadowPitch = 0;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    resource_view maskSrv = {};
    uint64_t frame = 0, lastFilled = 0;
    std::vector<ComPtr<ID3D12Resource>> retired[3]; // libérées trois images plus tard
    uint32_t photoIndex = 0;
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
    float reflStrength = 1.0f, smoothMin = 0.5f, f0 = 0.04f;
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
} live;
constexpr uint32_t kLiveMaxPairs = 256;
constexpr uint32_t kLiveSunBytes = 48 + 8 * 64; // soleil, bilan, nombre de cascades, 8 cascades (application, viewport)

// ------------------------------------------------------------------------------------------------
// étalonnage stocké : loi de profondeur, convention de la carte d'ombres, description de cette carte
// ------------------------------------------------------------------------------------------------

void LiveSaveCalibration()
{
    if (FILE* f = std::fopen((g_dir + "\\urti_calib.ini").c_str(), "w"))
    {
        std::fprintf(f, "; étalonnage URTI de ce jeu (relu au lancement ; F6 le refait)\n");
        std::fprintf(f, "depth_a=%.9g\ndepth_b=%.9g\nsun_sign=%d\nsun_map=%ux%u:%u\n", live.A, live.B, live.sunSign > 0 ? 1 : -1, live.sunMapWidth,
                     live.sunMapHeight, live.sunMapFormat);
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
    auto make = [&](const void* code, size_t size, int srvCount, ComPtr<ID3D12RootSignature>& rs, ComPtr<ID3D12PipelineState>& pso, uint32_t constants) {
        D3D12_ROOT_PARAMETER p[6] = {}; // jusqu'à 4 SRV, 1 UAV, constantes
        for (int i = 0; i < srvCount; i++)
        {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[i].Descriptor.ShaderRegister = i;
        }
        p[srvCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[srvCount + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[srvCount + 1].Constants.Num32BitValues = constants;
        D3D12_ROOT_SIGNATURE_DESC d = { UINT(srvCount + 2), p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(g_createRootSignature(device, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = rs.Get();
        cd.CS = { code, size };
        return SUCCEEDED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso)));
    };
    return make(g_live_convert, sizeof(g_live_convert), 2, live.convertRs, live.convertPso, 4) &&
           make(g_live_shadow, sizeof(g_live_shadow), 4, live.shadowRs, live.shadowPso, 16) &&
           make(g_live_sunfit, sizeof(g_live_sunfit), 3, live.fitRs, live.fitPso, 4) &&
           make(g_live_sunvote, sizeof(g_live_sunvote), 1, live.voteRs, live.votePso, 4) &&
           make(g_live_composite, sizeof(g_live_composite), 3, live.compositeRs, live.compositePso, 12) &&
           make(g_live_reflect, sizeof(g_live_reflect), 4, live.reflectRs, live.reflectPso, 16) &&
           make(g_live_depthfit, sizeof(g_live_depthfit), 2, live.depthfitRs, live.depthfitPso, 4);
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
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.NumDescs = 1;
    in.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    Retire(live.view), Retire(live.blas), Retire(live.blasScratch);
    live.view = UavBuffer(device, cap * 12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
    Retire(live.depthBuf), Retire(live.shadowBuf), Retire(live.mask);
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
        r = MakeBuffer(device, kLiveMaxPairs * 32ull, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    for (auto& r : live.sunReadback)
        r = MakeBuffer(device, 48, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
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
            float refl = 1.0f, channel = 1, invert = 0, smoothMin = 0.5f, f0 = 0.04f, debugReflect = 0, slot = 2, everywhere = 0, rays = 2;
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
                value("shadow_rays=", rays);
            }
            std::fclose(f);
            live.iniCombine = int(combine); // -1 : le mode vient du menu de ReShade (UrtiMode)
            g_liveShowMask = mask != 0, g_liveDebugGame = uint32_t(debug);
            g_liveGameBias = gameBias, g_liveRayBias = rayBias, g_liveSunRadius = sunRadius;
            live.reflStrength = refl, live.reflChannel = uint32_t(channel) & 3, live.reflInvert = invert != 0, live.smoothMin = smoothMin;
            live.f0 = f0, live.debugReflect = uint32_t(debugReflect), live.materialSlot = uint32_t(slot) & 7, live.reflEverywhere = everywhere != 0;
            live.shadowRays = std::clamp(uint32_t(rays), 1u, 4u);
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
    if (!g_liveCapture || !live.enabled || live.reflStrength <= 0 || !OurDevice(cmd->get_device()))
        return;
    uint64_t target = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (live.materialSlot < g_liveGbufferCount)
            target = g_liveGbuffer[live.materialSlot];
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        marksNow.swap(g_liveMarks);
        if (!g_liveSunKeys.empty())
            g_livePrevSunKeys.swap(g_liveSunKeys);
        g_liveSunKeys.clear();
        g_liveSunMarked = 0;
    }
    // bilan du suivi du soleil, lu trois images plus tard
    if (const float* r = nullptr; live.sunReadback[live.frame % 3] && SUCCEEDED(live.sunReadback[live.frame % 3]->Map(0, nullptr, (void**)&r)) && r)
    {
        if (live.calibrated)
        {
            live.sunFrames++, live.pairSum += uint64_t(r[4]);
            if (r[7] > 0)
            {
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
                live.sunFrames = live.sunAccepted = live.pairSum = 0;
            }
        }
        live.sunReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    if (live.lastPresentDraws == 0 && live.bound)
    {
        live.statNoDraw++;
        // image sans dessin du jeu (image générée par DLSS) : l'image d'ombres précédente reste
        Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(g.counters.Get(), 0, g.zeros.Get(), 0, kSlots * 8ull);
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
                std::fill(std::begin(live.timeSum), std::end(live.timeSum), 0.0);
                live.timeFrames = 0;
            }
        }
        live.timeReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    LiveTime(list.Get(), 0);
    // 1. sommets de l'écran vers l'espace caméra (NaN au-delà de ce qui a été rempli)
    Transition(list.Get(), g.so.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, srv);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, srv);
    list->SetComputeRootSignature(live.convertRs.Get());
    list->SetPipelineState(live.convertPso.Get());
    list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
    list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, live.view->GetGPUVirtualAddress());
    const struct { float p00, p11; uint32_t count, counterOffset; } cc = { live.p00, live.p11, uint32_t(live.capacity), uint32_t(slot * 8) };
    list->SetComputeRoot32BitConstants(3, 4, &cc, 0);
    list->Dispatch(UINT((live.capacity + 255) / 256), 1, 1);
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
    struct Pair { uint32_t cam, map, slots; float weight; float viewport[4]; };
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
                const float* v = marksNow[m].viewport;
                pairs.push_back({ i, m, marksNow[i].slot | (marksNow[m].slot << 8), w, { v[0], v[1], v[2], v[3] } });
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
            Transition(list.Get(), g.marks.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            list->SetComputeRootSignature(live.fitRs.Get());
            list->SetPipelineState(live.fitPso.Get());
            list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, g.marks->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, up->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.pairDirs->GetGPUVirtualAddress());
            const struct { float p00, p11; uint32_t count, slotBytes; } fc = { live.p00, live.p11, uint32_t(pairs.size()), uint32_t(kSlotBytes) };
            list->SetComputeRoot32BitConstants(4, 4, &fc, 0);
            list->Dispatch(UINT(pairs.size()), 1, 1);
            Transition(list.Get(), g.marks.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(list.Get(), live.pairDirs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.voteRs.Get());
            list->SetPipelineState(live.votePso.Get());
            list->SetComputeRootShaderResourceView(0, live.pairDirs->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(1, live.sunBuf->GetGPUVirtualAddress());
            const struct { uint32_t count; float cosTol, minShare, sunSign; } vc = { uint32_t(pairs.size()), std::cos(2.0f * 3.14159265f / 180.0f), 0.25f, live.sunSign };
            list->SetComputeRoot32BitConstants(2, 4, &vc, 0);
            list->Dispatch(1, 1, 1);
            Transition(list.Get(), live.pairDirs.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            // aucune paire : bilan « non mesuré », le soleil précédent reste, plus de cascade connue
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.sunBuf.Get(), 16, g.zeros.Get(), 0, 20); // pas la loi de profondeur (36 à 47)
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
        list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(2, live.sunBuf->GetGPUVirtualAddress());
        const uint32_t dc[4] = { uint32_t(slot * kSlotBytes), uint32_t(slot * 8), uint32_t(kSlotBytes), 0 };
        list->SetComputeRoot32BitConstants(3, 4, dc, 0);
        list->Dispatch(1, 1, 1);
    }
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);

    // 2. structures d'accélération
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer = { live.view->GetGPUVirtualAddress(), 12 };
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = UINT(live.capacity);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
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
    if (ID3D12Resource* atlas = reinterpret_cast<ID3D12Resource*>(g_liveSunDepth); atlas && g_liveCombine && live.calibrated)
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
    // 4. un rayon par pixel vers le soleil
    list->SetComputeRootSignature(live.shadowRs.Get());
    list->SetPipelineState(live.shadowPso.Get());
    list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, live.sunBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, atlasValid ? live.atlasBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, live.shadowBuf->GetGPUVirtualAddress());
    const bool d24 = depthDesc.Format == DXGI_FORMAT_R24G8_TYPELESS || depthDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    // soleil mesuré dans les 30 dernières images (3 de retard de lecture) : sinon pas d'ombres RT
    const bool sunNow = live.calibrated && live.framesSinceSun < 30;
    const uint32_t flags = (atlasValid ? 1u : 0u) | (live.atlasD24 ? 2u : 0u) | (g_liveCombine ? 4u : 0u) | (g_liveDebugGame ? 8u : 0u) |
                           (sunNow ? 0u : 16u) | (live.shadowRays << 8);
    const struct
    {
        float sunSign, gameBias, unused, bias;
        uint32_t width, height, depthPitch, shadowPitch;
        float A, B, p00, p11, clearDepth;
        uint32_t d24, atlasPitch, flags;
    } sc = { live.sunSign, g_liveGameBias, g_liveSunRadius * 3.14159265f / 180.0f, g_liveRayBias, live.width, live.height, live.depthPitch, live.shadowPitch,
             live.A, live.B, live.p00, live.p11, g.depthReversed ? 0.0f : 1.0f, d24 ? 1u : 0u, live.atlasPitch, flags };
    list->SetComputeRoot32BitConstants(5, 16, &sc, 0);
    list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
    if (atlasValid)
        Transition(list.Get(), live.atlasBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(live.sunReadback[live.frame % 3].Get(), 0, live.sunBuf.Get(), 0, 48);
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    LiveTime(list.Get(), 4);
    // 4b. reflets : un rayon réfléchi par pixel lisse (matériau du G-buffer copié dans cette image)
    if (live.reflStrength > 0 || live.debugReflect)
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
        if (live.reflectBuf && live.reflectPso)
        {
            if (live.materialBuf)
                Transition(list.Get(), live.materialBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.reflectRs.Get());
            list->SetPipelineState(live.reflectPso.Get());
            list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, live.materialBuf ? live.materialBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, live.reflectBuf->GetGPUVirtualAddress());
            const struct
            {
                uint32_t width, height, depthPitch, materialPitch;
                float A, B, p00, p11, clearDepth;
                uint32_t d24, channel;
                float smoothMin, bias;
                uint32_t flags;
                float f0;
                uint32_t reflectPitch;
            } rc = { live.width, live.height, live.depthPitch, live.materialPitch, live.A, live.B, live.p00, live.p11, g.depthReversed ? 0.0f : 1.0f,
                     d24 ? 1u : 0u, live.reflChannel, live.smoothMin, 0.002f, (material ? 1u : 0u) | (live.reflInvert ? 2u : 0u) | (live.reflEverywhere ? 4u : 0u), live.f0, live.width * 16 };
            list->SetComputeRoot32BitConstants(5, 16, &rc, 0);
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
    Transition(list.Get(), live.view.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);

    // 6. compteurs : lus plus tard, puis remis à zéro pour l'image suivante
    Transition(list.Get(), g.counters.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(live.counterReadback[live.frame % 3].Get(), 0, g.counters.Get(), 0, kSlots * 8ull);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(g.counters.Get(), 0, g.zeros.Get(), 0, kSlots * 8ull);
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
    // réglages de l'effet (menu de ReShade) : force, écran partagé, masque seul
    float strength = 0.55f;
    bool split = false, showMask = false;
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiStrength"); v.handle)
        runtime->get_uniform_value_float(v, &strength, 1);
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiSplit"); v.handle)
        runtime->get_uniform_value_bool(v, &split, 1);
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiShowMask"); v.handle)
        runtime->get_uniform_value_bool(v, &showMask, 1);
    showMask = showMask || g_liveShowMask;
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
    Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    const bool reflections = live.reflectBuf && (live.reflStrength > 0 || live.debugReflect);
    if (reflections)
        Transition(list.Get(), live.reflectBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootSignature(live.compositeRs.Get());
    list->SetPipelineState(live.compositePso.Get());
    list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, reflections ? live.reflectBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, live.imageOut->GetGPUVirtualAddress());
    const struct
    {
        uint32_t width, height, pitch, format, maskWidth, maskHeight, maskPitch;
        float strength;
        uint32_t split, showMask;
        float reflStrength;
        uint32_t debugReflect;
    } cc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.shadowPitch, strength,
             split ? 1u : 0u, showMask ? 1u : 0u, reflections ? live.reflStrength : 0.0f, reflections ? live.debugReflect : 0u };
    list->SetComputeRoot32BitConstants(4, 12, &cc, 0);
    list->Dispatch((cc.width + 7) / 8, (cc.height + 7) / 8, 1);
    Transition(list.Get(), live.shadowBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (reflections)
        Transition(list.Get(), live.reflectBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list.Get(), live.imageIn.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
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

// photo de l'écran après les effets (fichier déclencheur urti_photo)
void OnReshadePresent(effect_runtime* runtime)
{
    if (!OurDevice(runtime->get_device()))
        return;
    LiveComposite(runtime);
    if (++live.statPresents == 300)
    {
        Log("URTI direct : sur 300 présentations, %u sans dessin du jeu, %u passages dans les effets, %u avec ombres RT appliquées",
            live.statNoDraw, live.statEffects, live.statComposited);
        live.statPresents = live.statNoDraw = live.statEffects = live.statComposited = 0;
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

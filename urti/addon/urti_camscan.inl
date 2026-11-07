// URTI : la caméra du jeu, lue dans sa mémoire d'envoi (tampons visibles du processeur), au lieu d'être déduite de la géométrie.
//
// Lecteur (urti.ini camera_cb=512, relu au chargement de l'add-on) : Horizon Forbidden West écrit à chaque image, plusieurs
// fois, un tampon de constantes de 512 octets qui décrit sa caméra, avant de créer la vue dessus :
//   +240 projection, par colonnes : (p00 0 0 0) (0 p11 0 0) (jx jy a -1) (0 0 b 0), jx, jy : décalage sous-pixel du DLSS ;
//   +304 caméra -> monde, par colonnes : droite, haut, arrière, position.
// Chaque création de vue de cette taille est relue ; une valeur nouvelle prend un numéro de série. Le dessin capté retient le
// numéro en cours, et le calcul prend le plus récent des dessins de sa fenêtre : la caméra exacte de l'image captée.
//
// Cette vue peut disparaître : Horizon ne l'écrit plus si le joueur coupe ses reflets dans les options. Plus aucune caméra valable depuis 90
// présentations : les vues créées (une sur deux, 2 Mo par présentation au plus) et les tampons liés directement à la racine sont fouillés. Deux formes :
// une projection comme ci-dessus suivie d'une matrice caméra -> monde ou monde -> caméra, ou une matrice vue-projection accompagnée de sa matrice de vue
// (les dessins principaux, liés à la racine ; la projection, le décalage et la loi de profondeur s'en déduisent). Décalage sous-pixel non nul et changeant, vue à
// au moins 60 % des présentations pendant au moins trente : elle devient la nouvelle source.
//
// Sans ces reflets, aucune source sûre n'a été trouvée dans Horizon : il ne reste que les tampons des dessins (monde-vue-projection, monde-vue), où la vraie caméra n'est
// que la valeur la plus fréquente, et pas à chaque image (elle alterne avec le repère de gros objets : essai du 2026-11-07). Une caméra parfois fausse étant pire que la mesure sur
// les témoins, c'est celle-ci qui prend le relais (urti_live.inl). Une caméra ne se téléporte pas : une source qui saute est écartée, à l'adoption et ensuite.
//
// Recherche (fichier urti_camscan à côté de l'add-on) : à intervalles réguliers, un fil à part parcourt ces tampons à la
// recherche de matrices en forme de caméra, et relie chaque trouvaille à sa vue : c'est ainsi que le tampon ci-dessus a été
// trouvé, et ce qui servira pour les autres jeux.

struct GameCamera
{
    float p00 = 0, p11 = 0, jx = 0, jy = 0, a = 0, b = 0;
    float right[3] = {}, up[3] = {}, back[3] = {}, pos[3] = {};
};

struct CamBuffer
{
    ID3D12Resource* res;
    const uint8_t* ptr;
    uint64_t size;
};
struct CamBinding
{
    uint32_t param, kind; // kind 0 : racine, graphique ; 1 : racine, calcul ; 2 : vue créée
    uint64_t layout;
};

// lecteur
uint32_t g_camCbvSize = 0;
std::shared_mutex g_camVaMutex;
std::map<uint64_t, CamBuffer> g_camByVa;
std::unordered_map<uint64_t, uint64_t> g_camVaOfRes;
std::mutex g_camRingMutex;
GameCamera g_camRing[32];
alignas(16) float g_camLastRaw[32] = {};
std::atomic<uint32_t> g_camSerial{ 0 };
std::atomic<uint64_t> g_camRefused{ 0 }, g_camProbes{ 0 };
// source en cours : vue créée (taille) ou tampon lié à la racine (numéro du paramètre) ; forme : projection + matrice, ou vue-projection + vue
std::atomic<uint32_t> g_camSizeNow{ 0 }, g_camRootParam{ ~0u }, g_camOffA{ 240 }, g_camOffB{ 304 };
std::atomic<bool> g_camPairForm{ false }, g_camMatIsView{ false };
// recherche d'une autre source, quand il n'y a plus de caméra valable
std::atomic<uint32_t> g_camPresentIndex{ 0 }, g_camLastAccept{ 0 }, g_camViewCounter{ 0 };
std::atomic<bool> g_camSearching{ false };
std::atomic<int64_t> g_camSearchBudget{ 0 };
struct CamCandidate
{
    uint32_t count = 0, jitterChanges = 0, valueChanges = 0, jumps = 0, firstPresent = 0, lastPresent = 0, presents = 0;
    float lastJx = 0, lastJy = 0, lastPos[3] = {};
};
std::mutex g_camSearchMutex;
// racine ?, taille ou paramètre, place A, place B, forme (0 projection + caméra -> monde, 1 projection + monde -> caméra, 2 vue-projection + vue)
std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, CamCandidate> g_camCandidates;
std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_camProjOnly;                         // projection sans matrice : taille, place
std::set<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>> g_camRejected;        // sources adoptées puis écartées (valeurs trop nombreuses)
std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t> g_camAdopted;
uint32_t g_camCheckSerial = 0, g_camCheckPresent = 0;
std::atomic<uint32_t> g_camCheckJumps{ 0 }; // sauts de plus de 10 m de la source adoptée par la recherche
std::atomic<uint64_t> g_camSearchViews{ 0 };

// recherche
std::mutex g_camMutex;
std::vector<CamBuffer> g_camBuffers;
std::map<std::pair<uint64_t, uint64_t>, CamBinding> g_camBindings;
std::atomic<bool> g_camOn{ false }, g_camBusy{ false };
std::atomic<uint32_t> g_camPresents{ 0 }, g_camScanIndex{ 0 };
std::atomic<uint64_t> g_camRootBinds{ 0 }, g_camTableBinds{ 0 }, g_camViews{ 0 };

// lecture de mémoire combinée en écriture, protégée : le tampon a pu disparaître
bool CamRead(const uint8_t* src, uint8_t* dst, size_t n)
{
    __try
    {
        for (size_t i = 0; i + 16 <= n; i += 16)
            _mm_store_si128(reinterpret_cast<__m128i*>(dst + i), _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i))));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

inline float CamDot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// valeur de la caméra (projection par colonnes, puis caméra -> monde par colonnes) : gardée si elle en a bien la forme et si elle est nouvelle
void CamAccept(const float* f)
{
    constexpr float kZero = 1e-4f;
    const float p00 = f[0], p11 = f[5];
    const float* right = f + 16;
    const float* up = f + 20;
    const float* back = f + 24;
    const bool shape = p00 > 0.3f && p00 < 8.0f && p11 > 0.3f && p11 < 8.0f && p11 / p00 > 1.2f && p11 / p00 < 3.8f && std::fabs(f[1]) < kZero && std::fabs(f[2]) < kZero &&
                       std::fabs(f[3]) < kZero && std::fabs(f[4]) < kZero && std::fabs(f[6]) < kZero && std::fabs(f[7]) < kZero &&
                       f[11] == -1.0f && std::fabs(f[12]) < kZero && std::fabs(f[13]) < kZero && f[14] > 0.0f && std::fabs(f[15]) < kZero && std::fabs(f[19]) < kZero &&
                       std::fabs(f[23]) < kZero && std::fabs(f[27]) < kZero && f[31] == 1.0f && std::fabs(CamDot(right, right) - 1.0f) < 0.002f && std::fabs(CamDot(up, up) - 1.0f) < 0.002f &&
                       std::fabs(CamDot(back, back) - 1.0f) < 0.002f && std::fabs(CamDot(right, up)) < 0.002f && std::fabs(CamDot(right, back)) < 0.002f && std::fabs(CamDot(up, back)) < 0.002f;
    if (!shape)
    {
        g_camRefused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::mutex> lock(g_camRingMutex);
    if (std::memcmp(f, g_camLastRaw, sizeof(g_camLastRaw)) == 0)
        return;
    {
        const float dx = f[28] - g_camLastRaw[28], dy = f[29] - g_camLastRaw[29], dz = f[30] - g_camLastRaw[30];
        if (dx * dx + dy * dy + dz * dz > 100.0f)
            g_camCheckJumps.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(g_camLastRaw, f, sizeof(g_camLastRaw));
    const uint32_t serial = g_camSerial.load(std::memory_order_relaxed) + 1;
    GameCamera& c = g_camRing[serial % 32];
    c.p00 = p00, c.p11 = p11, c.jx = f[8], c.jy = f[9], c.a = f[10], c.b = f[14];
    std::memcpy(c.right, right, 12), std::memcpy(c.up, up, 12), std::memcpy(c.back, back, 12), std::memcpy(c.pos, f + 28, 12);
    g_camSerial.store(serial, std::memory_order_release);
    g_camLastAccept.store(g_camPresentIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// vue-projection et vue, en mémoire par colonnes -> projection et caméra -> monde. Clip = P V : x = p00 droite + jx arrière, y = p11 haut + jy arrière, z = a arrière + (0, 0, 0, b),
// w = -arrière. Faux si la première n'est pas le produit d'une projection du jeu par la seconde (les matrices des objets ont la même allure, mais pas leur vue à côté)
bool CamFromPair(const float* vp, const float* v, float* f)
{
    const float right[3] = { v[0], v[4], v[8] }, up[3] = { v[1], v[5], v[9] }, back[3] = { v[2], v[6], v[10] };
    const float r0[3] = { vp[0], vp[4], vp[8] }, r1[3] = { vp[1], vp[5], vp[9] }, r2[3] = { vp[2], vp[6], vp[10] }, r3[3] = { vp[3], vp[7], vp[11] };
    if (std::fabs(v[3]) > 1e-4f || std::fabs(v[7]) > 1e-4f || std::fabs(v[11]) > 1e-4f || v[15] != 1.0f)
        return false;
    if (std::fabs(CamDot(right, right) - 1.0f) > 0.002f || std::fabs(CamDot(up, up) - 1.0f) > 0.002f || std::fabs(CamDot(back, back) - 1.0f) > 0.002f ||
        std::fabs(CamDot(right, up)) > 0.002f || std::fabs(CamDot(right, back)) > 0.002f || std::fabs(CamDot(up, back)) > 0.002f)
        return false;
    // quatrième rangée : -arrière, translation comprise
    if (std::fabs(r3[0] + back[0]) > 2e-3f || std::fabs(r3[1] + back[1]) > 2e-3f || std::fabs(r3[2] + back[2]) > 2e-3f || std::fabs(vp[15] + v[14]) > 0.01f + 1e-3f * std::fabs(v[14]))
        return false;
    const float p00 = CamDot(r0, right), p11 = CamDot(r1, up), jx = CamDot(r0, back), jy = CamDot(r1, back), a = CamDot(r2, back);
    if (!(p00 > 0.3f && p00 < 8.0f && p11 > 0.3f && p11 < 8.0f && p11 / p00 > 1.2f && p11 / p00 < 3.8f) || std::fabs(jx) > 0.02f || std::fabs(jy) > 0.02f)
        return false;
    // (les deux autres composantes de chaque rangée sont nulles : pas de rotation en plus)
    if (std::fabs(CamDot(r0, up)) > 1e-3f || std::fabs(CamDot(r1, right)) > 1e-3f)
        return false;
    std::memset(f, 0, sizeof(float) * 32);
    f[0] = p00, f[5] = p11, f[8] = jx, f[9] = jy, f[10] = a, f[11] = -1.0f, f[14] = vp[14] - a * v[14];
    std::memcpy(f + 16, right, 12), std::memcpy(f + 20, up, 12), std::memcpy(f + 24, back, 12);
    f[28] = -(right[0] * v[12] + up[0] * v[13] + back[0] * v[14]);
    f[29] = -(right[1] * v[12] + up[1] * v[13] + back[1] * v[14]);
    f[30] = -(right[2] * v[12] + up[2] * v[13] + back[2] * v[14]);
    f[31] = 1.0f;
    return true;
}

// mémoire visible du processeur à cette adresse GPU, n octets lisibles
const uint8_t* CamCpuOfVa(uint64_t va, uint64_t n)
{
    std::shared_lock<std::shared_mutex> lock(g_camVaMutex);
    auto it = g_camByVa.upper_bound(va);
    if (it == g_camByVa.begin())
        return nullptr;
    --it;
    if (va + n > it->first + it->second.size)
        return nullptr;
    return it->second.ptr + (va - it->first);
}

// lecture de la source en cours
void CamProbeCpu(const uint8_t* cpu)
{
    alignas(16) float r[32];
    if (!CamRead(cpu + g_camOffA.load(std::memory_order_relaxed), reinterpret_cast<uint8_t*>(r), 64) || !CamRead(cpu + g_camOffB.load(std::memory_order_relaxed), reinterpret_cast<uint8_t*>(r + 16), 64))
        return;
    g_camProbes.fetch_add(1, std::memory_order_relaxed);
    if (g_camPairForm.load(std::memory_order_relaxed))
    {
        alignas(16) float f[32];
        if (CamFromPair(r, r + 16, f))
            CamAccept(f);
        else
            g_camRefused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // matrice monde -> caméra : retournée (rotation transposée, position)
    if (g_camMatIsView.load(std::memory_order_relaxed))
    {
        const float m[16] = { r[16], r[17], r[18], r[19], r[20], r[21], r[22], r[23], r[24], r[25], r[26], r[27], r[28], r[29], r[30], r[31] };
        r[16] = m[0], r[17] = m[4], r[18] = m[8];
        r[20] = m[1], r[21] = m[5], r[22] = m[9];
        r[24] = m[2], r[25] = m[6], r[26] = m[10];
        r[28] = -(r[16] * m[12] + r[20] * m[13] + r[24] * m[14]);
        r[29] = -(r[17] * m[12] + r[21] * m[13] + r[25] * m[14]);
        r[30] = -(r[18] * m[12] + r[22] * m[13] + r[26] * m[14]);
    }
    CamAccept(r);
}

void CamReaderProbe(uint64_t va)
{
    if (const uint8_t* cpu = CamCpuOfVa(va, g_camSizeNow.load(std::memory_order_relaxed)))
        CamProbeCpu(cpu);
}

// caméra de ce numéro (ou la plus récente, s'il est trop ancien)
GameCamera CamGet(uint32_t serial)
{
    std::lock_guard<std::mutex> lock(g_camRingMutex);
    const uint32_t last = g_camSerial.load(std::memory_order_relaxed);
    return g_camRing[(last - serial >= 24 ? last : serial) % 32];
}

// une présence de plus pour une forme trouvée ; assez souvent, longtemps, décalage changeant : elle devient la source (appelé sous g_camSearchMutex)
// Les tampons des objets ont la même allure quand l'objet est seulement tourné ; mais une caméra ne se téléporte pas : de sa valeur à la suivante, quelques centimètres,
// et très peu de valeurs par image, même liée des dizaines de fois
bool CamCandidateSeen(bool root, uint32_t key, uint32_t a, uint32_t b, uint32_t form, float jx, float jy, const float* pos)
{
    if (g_camRejected.count({ root, key, a, b, form }))
        return false;
    CamCandidate& c = g_camCandidates[{ root, key, a, b, form }];
    const uint32_t now = g_camPresentIndex.load(std::memory_order_relaxed);
    if (!c.count)
        c.firstPresent = now, c.lastPresent = now - 1;
    c.count++;
    if (jx != c.lastJx || jy != c.lastJy)
        c.jitterChanges++;
    c.lastJx = jx, c.lastJy = jy;
    if (pos[0] != c.lastPos[0] || pos[1] != c.lastPos[1] || pos[2] != c.lastPos[2])
    {
        const float dx = pos[0] - c.lastPos[0], dy = pos[1] - c.lastPos[1], dz = pos[2] - c.lastPos[2];
        if (c.count > 1 && dx * dx + dy * dy + dz * dz > 25.0f)
            c.jumps++;
        c.valueChanges++;
        c.lastPos[0] = pos[0], c.lastPos[1] = pos[1], c.lastPos[2] = pos[2];
    }
    if (c.lastPresent != now)
        c.presents++, c.lastPresent = now;
    if (c.presents < 30 || c.jitterChanges < 10 || c.presents * 10 < (now - c.firstPresent + 1) * 6 || c.valueChanges > c.presents * 4 || c.jumps > c.valueChanges / 50)
        return false;
    g_camAdopted = { root, key, a, b, form };
    g_camCheckSerial = g_camSerial.load(std::memory_order_relaxed), g_camCheckPresent = now, g_camCheckJumps.store(0, std::memory_order_relaxed);
    g_camOffA.store(a, std::memory_order_relaxed), g_camOffB.store(b, std::memory_order_relaxed);
    g_camPairForm.store(form == 2, std::memory_order_relaxed), g_camMatIsView.store(form == 1, std::memory_order_relaxed);
    g_camSizeNow.store(root ? 0 : key, std::memory_order_relaxed), g_camRootParam.store(root ? key : ~0u, std::memory_order_relaxed);
    g_camLastAccept.store(now, std::memory_order_relaxed);
    g_camSearching.store(false, std::memory_order_relaxed);
    static const char* const kForm[3] = { "projection + caméra -> monde", "projection + monde -> caméra", "vue-projection + vue" };
    Log("URTI : caméra du jeu retrouvée : %s %u, %s, +%u et +%u (vue %u fois, %u présentations)", root ? "tampon lié à la racine, paramètre" : "vues de taille", key, kForm[form], a, b, c.count, c.presents);
    g_camCandidates.clear();
    return true;
}

// un tampon quelconque, quand la caméra est perdue
void CamSearchMemory(const uint8_t* cpu, uint32_t size, bool root, uint32_t key)
{
    size = std::min<uint32_t>(size, 16384) & ~15u;
    if (size < 64 || g_camSearchBudget.fetch_sub(size, std::memory_order_relaxed) < int64_t(size))
        return;
    alignas(16) thread_local uint8_t buf[16384];
    if (!CamRead(cpu, buf, size))
        return;
    g_camSearchViews.fetch_add(1, std::memory_order_relaxed);
    const float* f = reinterpret_cast<const float*>(buf);
    const uint32_t count = size / 4;
    constexpr float kZero = 1e-4f;
    for (uint32_t i = 0; i + 16 <= count; i += 4)
    {
        const float* g = f + i;
        // vue-projection et sa vue, tout près (avant ou après)
        if (g[3] != 0.0f || g[7] != 0.0f || g[11] != 0.0f)
        {
            for (int d = -64; d <= 64; d += 128)
            {
                const int j = int(i) + d / 4;
                if (j < 0 || uint32_t(j) + 16 > count)
                    continue;
                alignas(16) float cam[32];
                if (!CamFromPair(g, f + j, cam) || (cam[8] == 0.0f && cam[9] == 0.0f))
                    continue;
                std::lock_guard<std::mutex> lock(g_camSearchMutex);
                if (CamCandidateSeen(root, key, i * 4, uint32_t(j) * 4, 2, cam[8], cam[9], cam + 28))
                    return;
            }
        }
        const float p00 = g[0], p11 = g[5];
        if (!(p00 > 0.3f && p00 < 8.0f && p11 > 0.3f && p11 < 8.0f && p11 / p00 > 1.2f && p11 / p00 < 3.8f) || g[11] != -1.0f || !(g[14] > 0.0f) ||
            std::fabs(g[1]) > kZero || std::fabs(g[2]) > kZero || std::fabs(g[3]) > kZero || std::fabs(g[4]) > kZero || std::fabs(g[6]) > kZero || std::fabs(g[7]) > kZero ||
            std::fabs(g[12]) > kZero || std::fabs(g[13]) > kZero || std::fabs(g[15]) > kZero)
            continue;
        const float jx = g[8], jy = g[9];
        if ((jx == 0.0f && jy == 0.0f) || std::fabs(jx) > 0.02f || std::fabs(jy) > 0.02f)
            continue; // pas la vue du jeu décalée par l'anticrénelage
        // matrice : d'abord après la projection, sinon avant
        int mat = -1;
        bool isView = false;
        auto test = [&](uint32_t j) {
            const float* m = f + j;
            if (std::fabs(m[3]) > kZero || std::fabs(m[7]) > kZero || std::fabs(m[11]) > kZero || m[15] != 1.0f)
                return false;
            const float* a = m, *b = m + 4, *c = m + 8;
            if (std::fabs(CamDot(a, a) - 1.0f) > 0.002f || std::fabs(CamDot(b, b) - 1.0f) > 0.002f || std::fabs(CamDot(c, c) - 1.0f) > 0.002f ||
                std::fabs(CamDot(a, b)) > 0.002f || std::fabs(CamDot(a, c)) > 0.002f || std::fabs(CamDot(b, c)) > 0.002f)
                return false;
            if (std::fabs(m[0]) > 0.9999f && std::fabs(m[5]) > 0.9999f)
                return false; // identité
            // la droite de la caméra est horizontale : une de ses composantes est nulle (colonnes : caméra -> monde ; lignes : monde -> caméra)
            const float byCol = std::min({ std::fabs(m[0]), std::fabs(m[1]), std::fabs(m[2]) }), byRow = std::min({ std::fabs(m[0]), std::fabs(m[4]), std::fabs(m[8]) });
            isView = byRow < byCol;
            return true;
        };
        for (uint32_t j = i + 16; j + 16 <= count && mat < 0; j += 4)
            if (test(j))
                mat = int(j);
        for (int j = int(i) - 16; j >= 0 && mat < 0; j -= 4)
            if (test(uint32_t(j)))
                mat = j;
        std::lock_guard<std::mutex> lock(g_camSearchMutex);
        if (mat < 0)
        {
            g_camProjOnly[{ size, i * 4 }]++;
            continue;
        }
        float pos[3] = { f[mat + 12], f[mat + 13], f[mat + 14] };
        if (isView)
        {
            const float* m = f + mat;
            pos[0] = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]);
            pos[1] = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
            pos[2] = -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]);
        }
        if (CamCandidateSeen(root, key, i * 4, uint32_t(mat) * 4, isView ? 1 : 0, jx, jy, pos))
            return;
    }
}

using PFN_CreateCbv = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
PFN_CreateCbv g_createCbv = nullptr;
void STDMETHODCALLTYPE HookCreateCbv(ID3D12Device* device, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    g_createCbv(device, desc, dest);
    if (!desc)
        return;
    if (desc->SizeInBytes == g_camSizeNow.load(std::memory_order_relaxed))
        CamReaderProbe(desc->BufferLocation);
    // caméra perdue : une vue sur deux fouillée, l'autre moitié à la présentation suivante
    if (g_camSearching.load(std::memory_order_relaxed) && desc->SizeInBytes >= 128 && desc->SizeInBytes <= 65536 &&
        ((g_camViewCounter.fetch_add(1, std::memory_order_relaxed) ^ g_camPresentIndex.load(std::memory_order_relaxed)) & 1) == 0)
        if (const uint8_t* cpu = CamCpuOfVa(desc->BufferLocation, std::min<uint32_t>(desc->SizeInBytes, 16384)))
            CamSearchMemory(cpu, desc->SizeInBytes, false, desc->SizeInBytes);
}

void CamReaderInstall(void* device)
{
    if (g_camCbvSize)
        PatchVtable(device, 17, reinterpret_cast<void*>(&HookCreateCbv), reinterpret_cast<void**>(&g_createCbv), "CreateConstantBufferView");
}

void CamScanNoteResource(const resource_desc& desc, resource res)
{
    if ((!g_camCbvSize && !g_camOn.load(std::memory_order_relaxed)) || desc.type != resource_type::buffer || desc.buffer.size < 4096)
        return;
    if (desc.heap != memory_heap::upload && desc.heap != memory_heap::custom && desc.heap != memory_heap::scratch)
        return;
    void* p = nullptr;
    ID3D12Resource* r = reinterpret_cast<ID3D12Resource*>(res.handle);
    if (FAILED(r->Map(0, nullptr, &p)) || !p)
        return;
    const CamBuffer entry = { r, static_cast<const uint8_t*>(p), desc.buffer.size };
    if (g_camCbvSize)
    {
        const uint64_t va = r->GetGPUVirtualAddress();
        std::unique_lock<std::shared_mutex> lock(g_camVaMutex);
        g_camByVa[va] = entry;
        g_camVaOfRes[res.handle] = va;
    }
    if (g_camOn.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(g_camMutex);
        g_camBuffers.push_back(entry);
    }
}

// tampon de constantes lié directement à la racine (pas de vue créée) : la source en cours, ou la recherche
void OnCamRootBind(command_list*, shader_stage, pipeline_layout, uint32_t param, const descriptor_table_update& u)
{
    if (u.type != descriptor_type::constant_buffer || !u.count)
        return;
    const bool searching = g_camSearching.load(std::memory_order_relaxed);
    if (!searching && g_camRootParam.load(std::memory_order_relaxed) != param)
        return;
    const buffer_range& b = *static_cast<const buffer_range*>(u.descriptors);
    uint64_t va = 0;
    {
        std::shared_lock<std::shared_mutex> lock(g_camVaMutex);
        const auto it = g_camVaOfRes.find(b.buffer.handle);
        if (it == g_camVaOfRes.end())
            return;
        va = it->second + b.offset;
    }
    if (g_camRootParam.load(std::memory_order_relaxed) == param)
        if (const uint8_t* cpu = CamCpuOfVa(va, 512))
            CamProbeCpu(cpu);
    if (searching)
    {
        const uint32_t size = b.size == UINT64_MAX ? 1024u : uint32_t(std::min<uint64_t>(b.size, 4096));
        if (const uint8_t* cpu = CamCpuOfVa(va, size))
            CamSearchMemory(cpu, size, true, param);
    }
}

void OnCamDestroyResource(device*, resource res)
{
    std::unique_lock<std::shared_mutex> lock(g_camVaMutex);
    if (const auto it = g_camVaOfRes.find(res.handle); it != g_camVaOfRes.end())
    {
        if (const auto b = g_camByVa.find(it->second); b != g_camByVa.end() && reinterpret_cast<uint64_t>(b->second.res) == res.handle)
            g_camByVa.erase(b);
        g_camVaOfRes.erase(it);
    }
}

void OnCamPush(command_list*, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update& u)
{
    if (u.type != descriptor_type::constant_buffer || !u.count)
        return;
    g_camRootBinds.fetch_add(1, std::memory_order_relaxed);
    const buffer_range& b = *static_cast<const buffer_range*>(u.descriptors);
    const uint32_t kind = (stages & shader_stage::all_compute) != 0 ? 1u : 0u;
    std::lock_guard<std::mutex> lock(g_camMutex);
    g_camBindings[{ b.buffer.handle, b.offset }] = { param, kind, layout.handle };
}

bool OnCamUpdate(device*, uint32_t count, const descriptor_table_update* updates)
{
    for (uint32_t i = 0; i < count; i++)
        if (updates[i].type == descriptor_type::constant_buffer && updates[i].count)
        {
            g_camViews.fetch_add(updates[i].count, std::memory_order_relaxed);
            const buffer_range* b = static_cast<const buffer_range*>(updates[i].descriptors);
            std::lock_guard<std::mutex> lock(g_camMutex);
            for (uint32_t k = 0; k < updates[i].count; k++)
                g_camBindings[{ b[k].buffer.handle, b[k].offset }] = { 0, 2, 0 };
        }
    return false;
}

void OnCamTables(command_list*, shader_stage, pipeline_layout, uint32_t, uint32_t count, const descriptor_table*, uint32_t, const uint32_t*)
{
    g_camTableBinds.fetch_add(count, std::memory_order_relaxed);
}

// 0 : rien ; 1 : vue-projection ; 2 : vue ; 3 : projection. r : les quatre rangées, chacune de quatre nombres
int CamClassify(const float r[4][4], float& p00, float& p11)
{
    const float n0 = CamDot(r[0], r[0]), n1 = CamDot(r[1], r[1]), n2 = CamDot(r[2], r[2]), n3 = CamDot(r[3], r[3]);
    if (!(n0 < 100.0f) || !(n1 < 100.0f) || !(n3 < 100.0f))
        return 0;
    // vue-projection : quatrième rangée = axe de la caméra (norme 1), deux premières orthogonales, de rapport l'écran
    if (std::fabs(n3 - 1.0f) < 0.02f && n0 > 0.09f && n1 > 0.09f)
    {
        const float l0 = std::sqrt(n0), l1 = std::sqrt(n1), ratio = l1 / l0;
        if (ratio > 1.5f && ratio < 2.0f && std::fabs(CamDot(r[0], r[1])) < 0.02f * l0 * l1 && std::fabs(CamDot(r[0], r[3])) < 0.02f * l0 &&
            std::fabs(CamDot(r[1], r[3])) < 0.02f * l1 && std::fabs(r[3][0]) < 0.9999f && std::fabs(r[3][1]) < 0.9999f && std::fabs(r[3][2]) < 0.9999f)
        {
            p00 = l0, p11 = l1;
            return 1;
        }
    }
    // vue : rotation pure, axes non alignés, sans roulis (une composante de la première rangée nulle)
    if (std::fabs(n0 - 1.0f) < 0.004f && std::fabs(n1 - 1.0f) < 0.004f && std::fabs(n2 - 1.0f) < 0.004f && std::fabs(CamDot(r[0], r[1])) < 0.002f &&
        std::fabs(CamDot(r[0], r[2])) < 0.002f && std::fabs(CamDot(r[1], r[2])) < 0.002f)
    {
        float big = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                big = std::max(big, std::fabs(r[i][j]));
        const float small0 = std::min({ std::fabs(r[0][0]), std::fabs(r[0][1]), std::fabs(r[0][2]) });
        if (big < 0.999f && small0 < 0.01f)
            return 2;
    }
    // projection seule : diagonale, rapport de l'écran, 1 ou -1 dans la dernière rangée
    if (r[0][0] > 0.3f && r[1][1] > 0.3f && r[1][1] / r[0][0] > 1.5f && r[1][1] / r[0][0] < 2.0f && std::fabs(r[0][1]) < 1e-5f && std::fabs(r[1][0]) < 1e-5f &&
        std::fabs(r[0][3]) < 1e-5f && std::fabs(r[1][3]) < 1e-5f && std::fabs(std::fabs(r[3][2]) - 1.0f) < 1e-4f && std::fabs(r[3][3]) < 1e-4f)
    {
        p00 = r[0][0], p11 = r[1][1];
        return 3;
    }
    return 0;
}

void CamScanThread(uint32_t index, uint32_t present)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<CamBuffer> buffers;
    {
        std::lock_guard<std::mutex> lock(g_camMutex);
        buffers = g_camBuffers;
    }
    const DWORD t0 = GetTickCount();
    constexpr size_t kChunk = 1u << 20;
    void* raw = _aligned_malloc(kChunk + 64, 64);
    uint8_t* chunk = static_cast<uint8_t*>(raw);
    uint64_t total = 0;
    uint32_t found[4] = {}, logged = 0;
    for (size_t bi = 0; bi < buffers.size(); bi++)
    {
        const CamBuffer& b = buffers[bi];
        total += b.size;
        for (uint64_t off = 0; off < b.size; off += kChunk)
        {
            const size_t n = size_t(std::min<uint64_t>(kChunk + 64, b.size - off)) & ~size_t(15);
            if (n < 64 || !CamRead(b.ptr + off, chunk, n))
                continue;
            for (size_t i = 0; i + 64 <= n; i += 16)
            {
                const float* m = reinterpret_cast<const float*>(chunk + i);
                for (int layout = 0; layout < 2; layout++)
                {
                    float r[4][4];
                    for (int a = 0; a < 4; a++)
                        for (int c = 0; c < 4; c++)
                            r[a][c] = layout == 0 ? m[a * 4 + c] : m[c * 4 + a];
                    float p00 = 0, p11 = 0;
                    const int kind = CamClassify(r, p00, p11);
                    if (!kind)
                        continue;
                    found[kind]++;
                    if (logged >= 160)
                        continue;
                    logged++;
                    long param = -1, kindB = -1;
                    int64_t dist = -1;
                    {
                        std::lock_guard<std::mutex> lock(g_camMutex);
                        auto it = g_camBindings.upper_bound({ uint64_t(reinterpret_cast<uintptr_t>(b.res)), off + i });
                        if (it != g_camBindings.begin())
                        {
                            --it;
                            if (it->first.first == uint64_t(reinterpret_cast<uintptr_t>(b.res)) && off + i - it->first.second < 65536)
                                param = long(it->second.param), kindB = long(it->second.kind), dist = int64_t(off + i - it->first.second);
                        }
                    }
                    Log("CAMSCAN %u pres=%u %s rang=%d tampon=%zu (%.0f Mo) pos=0x%llx lien=%ld/%ld+%lld p00=%.4f p11=%.4f champ=%.1f | %.4f %.4f %.4f %.3f | %.4f %.4f %.4f %.3f | %.4f %.4f %.4f %.3f | %.4f %.4f %.4f %.3f",
                        index, present, kind == 1 ? "VP" : kind == 2 ? "VUE" : "PROJ", layout, bi, b.size / 1048576.0, (unsigned long long)(off + i), kindB, param, (long long)dist,
                        p00, p11, p11 > 0 ? 2.0 * std::atan(1.0 / p11) * 57.29578 : 0.0, r[0][0], r[0][1], r[0][2], r[0][3], r[1][0], r[1][1], r[1][2], r[1][3], r[2][0], r[2][1], r[2][2],
                        r[2][3], r[3][0], r[3][1], r[3][2], r[3][3]);
                }
            }
        }
    }
    _aligned_free(raw);
    size_t bindings;
    {
        std::lock_guard<std::mutex> lock(g_camMutex);
        bindings = g_camBindings.size();
        g_camBindings.clear();
    }
    Log("CAMSCAN %u fini : %zu tampons, %.0f Mo, %.1f s ; VP %u, VUE %u, PROJ %u ; liaisons relevées %zu ; depuis le début : racine %llu, tables %llu, vues créées %llu", index, buffers.size(),
        total / 1048576.0, (GetTickCount() - t0) / 1000.0, found[1], found[2], found[3], bindings, (unsigned long long)g_camRootBinds.load(), (unsigned long long)g_camTableBinds.load(),
        (unsigned long long)g_camViews.load());
    g_camBusy.store(false);
}

void CamScanPresent()
{
    if (g_camCbvSize)
    {
        const uint32_t now = g_camPresentIndex.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool lost = now - g_camLastAccept.load(std::memory_order_relaxed) > 90;
        if (lost != g_camSearching.load(std::memory_order_relaxed))
        {
            Log("URTI : caméra du jeu %s", lost ? "perdue : recherche d'une autre vue" : "de nouveau lue");
            g_camSearching.store(lost, std::memory_order_relaxed);
        }
        // (peu : dans les menus aussi, la caméra est « perdue », et le menu tourne à 300 images par seconde)
        g_camSearchBudget.store(512 * 1024, std::memory_order_relaxed);
        // source adoptée par la recherche : plus de six caméras par présentation, ce n'est pas une caméra
        {
            std::lock_guard<std::mutex> lock(g_camSearchMutex);
            if (std::get<1>(g_camAdopted) && now - g_camCheckPresent >= 60)
            {
                const uint32_t serial = g_camSerial.load(std::memory_order_relaxed);
                const uint32_t jumps = g_camCheckJumps.exchange(0, std::memory_order_relaxed);
                if (serial - g_camCheckSerial > (now - g_camCheckPresent) * 6 || jumps > 3)
                {
                    Log("URTI : source de caméra écartée (%u valeurs, %u sauts en %u présentations) : tampon %u, +%u et +%u", serial - g_camCheckSerial, jumps, now - g_camCheckPresent, std::get<1>(g_camAdopted),
                        std::get<2>(g_camAdopted), std::get<3>(g_camAdopted));
                    g_camRejected.insert(g_camAdopted);
                    g_camAdopted = {};
                    g_camSizeNow.store(0, std::memory_order_relaxed), g_camRootParam.store(~0u, std::memory_order_relaxed);
                    g_camLastAccept.store(now - 1000, std::memory_order_relaxed);
                }
                g_camCheckSerial = serial, g_camCheckPresent = now;
            }
        }
        // bilan de la recherche, pour comprendre si elle ne trouve rien
        if (lost && now % 600 == 0)
        {
            std::lock_guard<std::mutex> lock(g_camSearchMutex);
            char text[400] = "";
            size_t n = 0;
            for (const auto& [key, c] : g_camProjOnly)
                if (n < sizeof(text) - 48)
                    n += std::snprintf(text + n, sizeof(text) - n, " %u+%u:%u", key.first, key.second, c);
            Log("URTI : recherche de la caméra : %llu vues lues ; projections sans matrice (taille+place:fois)%s ; %zu candidates", (unsigned long long)g_camSearchViews.load(), text, g_camCandidates.size());
            g_camProjOnly.clear();
            // les plus présentes
            std::vector<std::pair<uint32_t, const decltype(g_camCandidates)::value_type*>> top;
            for (const auto& e : g_camCandidates)
                top.push_back({ e.second.presents, &e });
            std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            uint32_t shown = 0, shownRoot = 0;
            for (size_t k = 0; k < top.size(); k++)
            {
                const auto& [key, c] = *top[k].second;
                uint32_t& quota = std::get<0>(key) ? shownRoot : shown;
                if (quota >= 8)
                    continue;
                quota++;
                Log("URTI : candidate %s %u, +%u et +%u, forme %u : %u fois, %u présentations sur %u, %u valeurs, %u sauts", std::get<0>(key) ? "racine" : "vues", std::get<1>(key), std::get<2>(key), std::get<3>(key),
                    std::get<4>(key), c.count, c.presents, now - c.firstPresent + 1, c.valueChanges, c.jumps);
            }
        }
    }
    if (!g_camOn.load(std::memory_order_relaxed))
        return;
    const uint32_t p = g_camPresents.fetch_add(1, std::memory_order_relaxed);
    if (p % 1200 != 1199 || g_camBusy.exchange(true))
        return;
    std::thread(CamScanThread, g_camScanIndex.fetch_add(1), p).detach();
}

void CamScanRegister()
{
    // le lecteur : réglage lu ici, les tampons d'envoi du jeu sont créés dès son démarrage
    std::ifstream ini(g_dir + "\\urti.ini");
    for (std::string line; std::getline(ini, line);)
        if (line.rfind("camera_cb=", 0) == 0)
            g_camCbvSize = uint32_t(std::strtoul(line.c_str() + 10, nullptr, 10));
    g_camSizeNow.store(g_camCbvSize);
    if (g_camCbvSize)
    {
        reshade::register_event<reshade::addon_event::destroy_resource>(OnCamDestroyResource);
        reshade::register_event<reshade::addon_event::push_descriptors>(OnCamRootBind);
        Log("URTI : caméra du jeu lue dans ses tampons de %u octets", g_camCbvSize);
    }
    if (GetFileAttributesA((g_dir + "\\urti_camscan").c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    g_camOn.store(true);
    reshade::register_event<reshade::addon_event::push_descriptors>(OnCamPush);
    reshade::register_event<reshade::addon_event::update_descriptor_tables>(OnCamUpdate);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(OnCamTables);
    Log("CAMSCAN : recherche de la caméra du jeu active");
}

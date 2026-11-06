// URTI : la caméra du jeu, lue dans sa mémoire d'envoi (tampons visibles du processeur), au lieu d'être déduite de la géométrie.
//
// Lecteur (urti.ini camera_cb=512, relu au chargement de l'add-on) : Horizon Forbidden West écrit à chaque image, plusieurs
// fois, un tampon de constantes de 512 octets qui décrit sa caméra, avant de créer la vue dessus :
//   +240 projection, par colonnes : (p00 0 0 0) (0 p11 0 0) (jx jy a -1) (0 0 b 0), jx, jy : décalage sous-pixel du DLSS ;
//   +304 caméra -> monde, par colonnes : droite, haut, arrière, position.
// Chaque création de vue de cette taille est relue ; une valeur nouvelle prend un numéro de série. Le dessin capté retient le
// numéro en cours, et le calcul prend le plus récent des dessins de sa fenêtre : la caméra exacte de l'image captée.
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

void CamReaderProbe(uint64_t va)
{
    const uint8_t* cpu = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(g_camVaMutex);
        auto it = g_camByVa.upper_bound(va);
        if (it == g_camByVa.begin())
            return;
        --it;
        if (va + g_camCbvSize > it->first + it->second.size)
            return;
        cpu = it->second.ptr + (va - it->first);
    }
    alignas(16) float f[32];
    if (!CamRead(cpu + 240, reinterpret_cast<uint8_t*>(f), sizeof(f)))
        return;
    g_camProbes.fetch_add(1, std::memory_order_relaxed);
    constexpr float kZero = 1e-4f;
    const float p00 = f[0], p11 = f[5];
    const float* right = f + 16;
    const float* up = f + 20;
    const float* back = f + 24;
    const bool shape = p00 > 0.3f && p00 < 8.0f && p11 > 0.3f && p11 < 8.0f && p11 / p00 > 1.2f && p11 / p00 < 3.8f && std::fabs(f[1]) < kZero && std::fabs(f[2]) < kZero &&
                       std::fabs(f[3]) < kZero && std::fabs(f[4]) < kZero && std::fabs(f[6]) < kZero && std::fabs(f[7]) < kZero && std::fabs(f[8]) < 0.01f && std::fabs(f[9]) < 0.01f &&
                       f[11] == -1.0f && std::fabs(f[12]) < kZero && std::fabs(f[13]) < kZero && f[14] > 0.0f && std::fabs(f[15]) < kZero && std::fabs(f[19]) < kZero &&
                       std::fabs(f[23]) < kZero && std::fabs(f[27]) < kZero && f[31] == 1.0f && std::fabs(CamDot(right, right) - 1.0f) < 0.002f && std::fabs(CamDot(up, up) - 1.0f) < 0.002f &&
                       std::fabs(CamDot(back, back) - 1.0f) < 0.002f && std::fabs(CamDot(right, up)) < 0.002f && std::fabs(CamDot(right, back)) < 0.002f && std::fabs(CamDot(up, back)) < 0.002f;
    if (!shape)
    {
        g_camRefused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::mutex> lock(g_camRingMutex);
    if (std::memcmp(f, g_camLastRaw, sizeof(f)) == 0)
        return;
    std::memcpy(g_camLastRaw, f, sizeof(f));
    const uint32_t serial = g_camSerial.load(std::memory_order_relaxed) + 1;
    GameCamera& c = g_camRing[serial % 32];
    c.p00 = p00, c.p11 = p11, c.jx = f[8], c.jy = f[9], c.a = f[10], c.b = f[14];
    std::memcpy(c.right, right, 12), std::memcpy(c.up, up, 12), std::memcpy(c.back, back, 12), std::memcpy(c.pos, f + 28, 12);
    g_camSerial.store(serial, std::memory_order_release);
}

// caméra de ce numéro (ou la plus récente, s'il est trop ancien)
GameCamera CamGet(uint32_t serial)
{
    std::lock_guard<std::mutex> lock(g_camRingMutex);
    const uint32_t last = g_camSerial.load(std::memory_order_relaxed);
    return g_camRing[(last - serial >= 24 ? last : serial) % 32];
}

using PFN_CreateCbv = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
PFN_CreateCbv g_createCbv = nullptr;
void STDMETHODCALLTYPE HookCreateCbv(ID3D12Device* device, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    g_createCbv(device, desc, dest);
    if (desc && desc->SizeInBytes == g_camCbvSize)
        CamReaderProbe(desc->BufferLocation);
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
    if (g_camCbvSize)
    {
        reshade::register_event<reshade::addon_event::destroy_resource>(OnCamDestroyResource);
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

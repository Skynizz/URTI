// URTI : débruiteurs de NVIDIA (NRD, couche d'intégration sur NRI) — inclus dans urti_live.inl.
//
// SIGMA débruite les ombres du soleil comme dans les jeux RTX : à partir d'un rayon par pixel et de la
// distance de l'obstacle (la pénombre), il reconstruit des ombres douces et stables d'une image à l'autre,
// guidé par la profondeur, les normales et les vecteurs de mouvement du jeu.
//
// Déroulé par image (OnBeginEffects) :
//  1. live_shadow (mode SIGMA) : pénombre d'un rayon par pixel + poids des corrections d'URTI ;
//  2. live_nrdprep : profondeur linéaire, normales encodées comme NRD l'attend ; copies vers les textures
//     de NRD (vecteurs de mouvement du jeu pris tels quels, l'échelle suit leur convention) ;
//  3. la liste de commandes de ReShade est soumise ; NRD enregistre ses passes sur une liste à lui (il
//     installe ses propres tables de descripteurs : rien ne fuit dans l'état de ReShade) ;
//  4. live_nrdpost : masque d'ombres = visibilité de SIGMA, puis les corrections (ombre du jeu, N·L, brume).
//
// L'espace « monde » donné à NRD est l'espace caméra de chaque image (matrices identité) : les mouvements
// passent par les vecteurs de mouvement du jeu.

#if URTI_NRD
// (en-tetes de NRD et NRI inclus au debut de urti_addon.cpp, hors de l'espace de noms anonyme)
#include "live_nrdpost_cs.h" // g_live_nrdpost
#include "live_nrdprep_cs.h" // g_live_nrdprep

struct NrdState
{
    nrd::Integration* integration = nullptr; // détruit avec l'appareil (jamais à la sortie du processus)
    bool failed = false, restart = true;
    uint32_t width = 0, height = 0, frameIndex = 0, sigmaPitch = 0;
    ComPtr<ID3D12Resource> mvTex, normalTex, viewZTex, penumbraTex, shadowTex; // textures de NRD
    ComPtr<ID3D12Resource> viewZBuf, normalBuf, penumbraBuf, sigmaBuf;        // tampons d'URTI
    ComPtr<ID3D12CommandAllocator> allocators[3];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0, allocatorFence[3] = {};
    HANDLE event = nullptr;
    ComPtr<ID3D12RootSignature> prepRs, postRs;
    ComPtr<ID3D12PipelineState> prepPso, postPso;
    double timeSum = 0;
};
NrdState g_nrd;

void NrdDestroy()
{
    if (g_nrd.integration)
    {
        g_nrd.integration->Destroy();
        delete g_nrd.integration;
    }
    if (g_nrd.event)
        CloseHandle(g_nrd.event);
    g_nrd = NrdState();
}

ComPtr<ID3D12Resource> NrdTexture(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format, bool storage, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = storage ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// pipeline de calcul à descripteurs racine : SRV t0…, UAV u0…, constantes b0
bool NrdPipeline(const void* code, size_t size, int srvCount, int uavCount, uint32_t constants, ComPtr<ID3D12RootSignature>& rs, ComPtr<ID3D12PipelineState>& pso)
{
    D3D12_ROOT_PARAMETER p[8] = {};
    for (int i = 0; i < srvCount; i++)
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV, p[i].Descriptor.ShaderRegister = i;
    for (int i = 0; i < uavCount; i++)
        p[srvCount + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV, p[srvCount + i].Descriptor.ShaderRegister = i;
    p[srvCount + uavCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[srvCount + uavCount].Constants.Num32BitValues = constants;
    D3D12_ROOT_SIGNATURE_DESC d = { UINT(srvCount + uavCount + 1), p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g_createRootSignature(g.device, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
    cd.pRootSignature = rs.Get();
    cd.CS = { code, size };
    return SUCCEEDED(g.device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso)));
}

// textures, tampons, liste de commandes et instance de NRD à la taille de rendu
bool NrdEnsure(effect_runtime* runtime)
{
    if (g_nrd.failed)
        return false;
    if (g_nrd.integration && g_nrd.width == live.width && g_nrd.height == live.height)
        return true;
    ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
    const uint32_t w = live.width, h = live.height;
    auto fail = [](const char* what) {
        Log("URTI direct : débruiteur NVIDIA (SIGMA) indisponible : %s ; débruitage d'URTI à la place", what);
        g_nrd.failed = true;
        return false;
    };
    if (!g_nrd.prepPso && (!NrdPipeline(g_live_nrdprep, sizeof(g_live_nrdprep), 2, 2, 12, g_nrd.prepRs, g_nrd.prepPso) ||
                           !NrdPipeline(g_live_nrdpost, sizeof(g_live_nrdpost), 2, 1, 8, g_nrd.postRs, g_nrd.postPso)))
        return fail("shaders refusés");
    if (!g_nrd.list)
    {
        for (auto& a : g_nrd.allocators)
            if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
                return fail("liste de commandes");
        if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_nrd.allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_nrd.list))) ||
            FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_nrd.fence))))
            return fail("liste de commandes");
        g_nrd.list->Close();
        g_nrd.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    // entrées (recopiées depuis nos tampons à chaque image) et sortie (aussi l'historique de SIGMA)
    g_nrd.mvTex = NrdTexture(g.device, w, h, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    g_nrd.normalTex = NrdTexture(g.device, w, h, DXGI_FORMAT_R10G10B10A2_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
    g_nrd.viewZTex = NrdTexture(g.device, w, h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    g_nrd.penumbraTex = NrdTexture(g.device, w, h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    g_nrd.shadowTex = NrdTexture(g.device, w, h, DXGI_FORMAT_R8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint64_t floats = uint64_t(live.shadowPitch) * h;
    g_nrd.viewZBuf = UavBuffer(g.device, floats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_nrd.normalBuf = UavBuffer(g.device, floats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_nrd.penumbraBuf = UavBuffer(g.device, floats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_nrd.sigmaPitch = (w + 255) & ~255u;
    g_nrd.sigmaBuf = UavBuffer(g.device, uint64_t(g_nrd.sigmaPitch) * h, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_nrd.mvTex || !g_nrd.normalTex || !g_nrd.viewZTex || !g_nrd.penumbraTex || !g_nrd.shadowTex || !g_nrd.viewZBuf || !g_nrd.normalBuf ||
        !g_nrd.penumbraBuf || !g_nrd.sigmaBuf)
        return fail("mémoire");
    if (!g_nrd.integration)
        g_nrd.integration = new nrd::Integration();
    nrd::IntegrationCreationDesc idesc = {};
    std::strcpy(idesc.name, "URTI");
    idesc.resourceWidth = uint16_t(w), idesc.resourceHeight = uint16_t(h);
    idesc.queuedFrameNum = 3;
    idesc.autoWaitForIdle = true;
    const nrd::DenoiserDesc denoisers[] = { { 0, nrd::Denoiser::SIGMA_SHADOW } };
    nrd::InstanceCreationDesc instance = {};
    instance.denoisers = denoisers, instance.denoisersNum = 1;
    nri::QueueFamilyD3D12Desc family = {};
    family.d3d12Queues = &queue, family.queueNum = 1, family.queueType = nri::QueueType::GRAPHICS;
    nri::DeviceCreationD3D12Desc dd = {};
    dd.d3d12Device = g.device;
    dd.queueFamilies = &family, dd.queueFamilyNum = 1;
    dd.disableD3D12EnhancedBarriers = true; // le jeu et URTI utilisent les barrières classiques
    dd.disableNVAPIInitialization = true;
    if (g_nrd.integration->RecreateD3D12(idesc, instance, dd) != nrd::Result::SUCCESS)
        return fail("création de l'instance NRD");
    g_nrd.width = w, g_nrd.height = h, g_nrd.restart = true;
    Log("URTI direct : débruiteur d'ombres NVIDIA SIGMA (NRD %u.%u.%u) prêt, %ux%u, %.0f Mo", NRD_VERSION_MAJOR, NRD_VERSION_MINOR, NRD_VERSION_BUILD, w,
        h, g_nrd.integration->GetTotalMemoryUsageInMb());
    return true;
}

// plantage dans NRD / NRI : fonction fautive et pile d'appels dans le journal (symboles de l'add-on), puis
// débruitage d'URTI à la place (le jeu continue)
LONG NrdCrashFilter(EXCEPTION_POINTERS* e)
{
    HANDLE process = GetCurrentProcess();
    static bool symbols = false;
    if (!symbols)
        symbols = SymInitialize(process, (g_dir + ";F:/NewrAYTRACING/urti/build/Release").c_str(), TRUE) != FALSE;
    Log("URTI direct : plantage dans le débruiteur NVIDIA (code 0x%08lx à %p)", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    CONTEXT context = *e->ContextRecord;
    for (int i = 0; i < 24 && context.Rip; i++)
    {
        char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO), sym->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
        DWORD lineDisplacement = 0;
        const bool named = SymFromAddr(process, context.Rip, &displacement, sym) != FALSE;
        const bool lined = SymGetLineFromAddr64(process, context.Rip, &lineDisplacement, &line) != FALSE;
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(context.Rip), &module);
        if (module)
            GetModuleFileNameA(module, path, MAX_PATH);
        const char* file = std::strrchr(path, '\\');
        Log("  %d %s+0x%llx %s +0x%llx %s:%lu", i, file ? file + 1 : path, (unsigned long long)(context.Rip - reinterpret_cast<DWORD64>(module)),
            named ? sym->Name : "?", (unsigned long long)displacement, lined ? line.FileName : "", lined ? line.LineNumber : 0);
        // déroulement x64 natif (tables d'exceptions des modules)
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!fn)
        {
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, fn, &context, &handlerData, &establisher, nullptr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

bool NrdDenoiseGuarded(const nrd::Identifier* ids, uint32_t count, const nri::CommandBufferD3D12Desc* cbd, nrd::ResourceSnapshot* snapshot)
{
    __try
    {
        g_nrd.integration->DenoiseD3D12(ids, count, *cbd, *snapshot);
        return true;
    }
    __except (NrdCrashFilter(GetExceptionInformation()))
    {
        return false;
    }
}

// tampon aux lignes alignées -> texture
void NrdCopyIn(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer, uint32_t pitch, ID3D12Resource* texture, DXGI_FORMAT format)
{
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = { format, live.width, live.height, 1, pitch };
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

// ombres RT débruitées par SIGMA dans live.shadowBuf ; shadowRawBuf contient les poids (live_shadow en
// mode SIGMA), g_nrd.penumbraBuf la pénombre. Faux : rien n'a été fait (débruitage d'URTI à la place).
bool NrdShadows(effect_runtime* runtime, ID3D12GraphicsCommandList4* list, bool d24, bool motionOk)
{
    if (!motionOk || !g_nrd.integration || g_nrd.width != live.width || g_nrd.height != live.height)
        return false;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // 2. profondeur linéaire et normales
    Transition(list, live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootSignature(g_nrd.prepRs.Get());
    list->SetPipelineState(g_nrd.prepPso.Get());
    list->SetComputeRootShaderResourceView(0, live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.sunBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, g_nrd.viewZBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, g_nrd.normalBuf->GetGPUVirtualAddress());
    const struct
    {
        uint32_t width, height, depthPitch, outPitch;
        float A, B, p00, p11;
        uint32_t depth24;
        float skyViewZ;
        uint32_t pad0, pad1;
    } pc = { live.width, live.height, live.depthPitch, live.shadowPitch, live.A, live.B, live.p00, live.p11, d24 ? 1u : 0u, 1e6f, 0, 0 };
    list->SetComputeRoot32BitConstants(4, 12, &pc, 0);
    list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
    Transition(list, live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* bufs[3] = { g_nrd.viewZBuf.Get(), g_nrd.normalBuf.Get(), g_nrd.penumbraBuf.Get() };
    for (ID3D12Resource* b : bufs)
        Transition(list, b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    NrdCopyIn(list, g_nrd.viewZBuf.Get(), live.shadowPitch, g_nrd.viewZTex.Get(), DXGI_FORMAT_R32_FLOAT);
    NrdCopyIn(list, g_nrd.normalBuf.Get(), live.shadowPitch, g_nrd.normalTex.Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
    NrdCopyIn(list, g_nrd.penumbraBuf.Get(), live.shadowPitch, g_nrd.penumbraTex.Get(), DXGI_FORMAT_R32_FLOAT);
    // vecteurs de mouvement du jeu, tels quels (en lecture pour les ombres à ce moment de l'image)
    Transition(list, live.mvBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    NrdCopyIn(list, live.mvBuf.Get(), live.mvPitch, g_nrd.mvTex.Get(), DXGI_FORMAT_R16G16_FLOAT);
    Transition(list, live.mvBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, srv);
    for (ID3D12Resource* b : bufs)
        Transition(list, b, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* inputs[4] = { g_nrd.mvTex.Get(), g_nrd.normalTex.Get(), g_nrd.viewZTex.Get(), g_nrd.penumbraTex.Get() };
    for (ID3D12Resource* t : inputs)
        Transition(list, t, D3D12_RESOURCE_STATE_COPY_DEST, srv);

    // 3. NRD sur sa propre liste, après ce qui précède
    runtime->get_command_queue()->flush_immediate_command_list();
    const uint32_t k = g_nrd.frameIndex % 3;
    if (g_nrd.fence->GetCompletedValue() < g_nrd.allocatorFence[k])
    {
        g_nrd.fence->SetEventOnCompletion(g_nrd.allocatorFence[k], g_nrd.event);
        WaitForSingleObject(g_nrd.event, 1000);
    }
    g_nrd.allocators[k]->Reset();
    g_nrd.list->Reset(g_nrd.allocators[k].Get(), nullptr);
    g_nrd.integration->NewFrame();
    nrd::CommonSettings cs = {};
    // projection supposée d'URTI (profondeur inversée, plan lointain à l'infini), colonne par colonne
    cs.viewToClipMatrix[0] = live.p00, cs.viewToClipMatrix[5] = live.p11, cs.viewToClipMatrix[11] = 1.0f, cs.viewToClipMatrix[14] = 0.1f;
    std::memcpy(cs.viewToClipMatrixPrev, cs.viewToClipMatrix, sizeof(cs.viewToClipMatrix));
    for (int i = 0; i < 4; i++)
        cs.worldToViewMatrix[i * 5] = cs.worldToViewMatrixPrev[i * 5] = 1.0f;
    // vecteurs de mouvement : « précédent - courant » en uv ; la convention du jeu (live_mvcal) donne l'échelle
    const uint32_t mvc = live.mvConvention;
    const float base = (mvc & 3) == 0 ? 1.0f : (mvc & 3) == 1 ? 0.5f : (mvc & 3) == 3 ? 2.0f : 0.0f;
    float sx = (mvc & 3) == 2 ? 1.0f / live.width : base, sy = (mvc & 3) == 2 ? 1.0f / live.height : base;
    if (mvc & 4)
        sx = -sx, sy = -sy;
    if (mvc & 8)
        sy = -sy;
    cs.motionVectorScale[0] = sx, cs.motionVectorScale[1] = sy, cs.motionVectorScale[2] = 0.0f;
    cs.resourceSize[0] = cs.resourceSizePrev[0] = cs.rectSize[0] = cs.rectSizePrev[0] = uint16_t(live.width);
    cs.resourceSize[1] = cs.resourceSizePrev[1] = cs.rectSize[1] = cs.rectSizePrev[1] = uint16_t(live.height);
    cs.denoisingRange = 1000.0f;
    cs.disocclusionThreshold = live.nrdDisocclusion;
    cs.frameIndex = g_nrd.frameIndex;
    cs.accumulationMode = g_nrd.restart ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    g_nrd.integration->SetCommonSettings(cs);
    nrd::SigmaSettings ss = {};
    const float* sun = (live.lastSun[0] || live.lastSun[1] || live.lastSun[2]) ? live.lastSun : live.sun;
    ss.lightDirection[0] = sun[0], ss.lightDirection[1] = sun[1], ss.lightDirection[2] = sun[2];
    ss.maxStabilizedFrameNum = uint32_t(std::clamp(live.nrdFrames, 0.0f, float(nrd::SIGMA_MAX_HISTORY_FRAME_NUM)));
    g_nrd.integration->SetDenoiserSettings(0, &ss);
    auto resource = [](ID3D12Resource* r, DXGI_FORMAT format, bool storage) {
        nrd::Resource x = {};
        x.d3d12.resource = r;
        x.d3d12.format = DXGIFormat(format);
        x.state = storage ? nri::AccessLayoutStage{ nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER }
                          : nri::AccessLayoutStage{ nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::COMPUTE_SHADER };
        return x;
    };
    nrd::ResourceSnapshot snapshot = {};
    snapshot.restoreInitialState = true;
    snapshot.SetResource(nrd::ResourceType::IN_MV, resource(g_nrd.mvTex.Get(), DXGI_FORMAT_R16G16_FLOAT, false));
    snapshot.SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS, resource(g_nrd.normalTex.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, false));
    snapshot.SetResource(nrd::ResourceType::IN_VIEWZ, resource(g_nrd.viewZTex.Get(), DXGI_FORMAT_R32_FLOAT, false));
    snapshot.SetResource(nrd::ResourceType::IN_PENUMBRA, resource(g_nrd.penumbraTex.Get(), DXGI_FORMAT_R32_FLOAT, false));
    snapshot.SetResource(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, resource(g_nrd.shadowTex.Get(), DXGI_FORMAT_R8_UNORM, true));
    const nrd::Identifier ids[] = { 0 };
    nri::CommandBufferD3D12Desc cbd = {};
    cbd.d3d12CommandList = g_nrd.list.Get();
    if (!NrdDenoiseGuarded(ids, 1, &cbd, &snapshot))
    {
        g_nrd.list->Close();
        g_nrd.failed = true;
        Log("URTI direct : débruiteur NVIDIA coupé après un plantage ; débruitage d'URTI à la place");
        return false;
    }
    g_nrd.list->Close();
    ID3D12CommandList* lists[] = { g_nrd.list.Get() };
    ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g_nrd.fence.Get(), ++g_nrd.fenceValue);
    g_nrd.allocatorFence[k] = g_nrd.fenceValue;
    g_nrd.frameIndex++, g_nrd.restart = false;

    // 4. retour sur la liste de ReShade : sortie de SIGMA -> masque d'ombres
    for (ID3D12Resource* t : inputs)
        Transition(list, t, srv, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list, g_nrd.shadowTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = g_nrd.sigmaBuf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UNORM, live.width, live.height, 1, g_nrd.sigmaPitch };
        src.pResource = g_nrd.shadowTex.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(list, g_nrd.shadowTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, g_nrd.sigmaBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    Transition(list, live.shadowRawBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootSignature(g_nrd.postRs.Get());
    list->SetPipelineState(g_nrd.postPso.Get());
    list->SetComputeRootShaderResourceView(0, g_nrd.sigmaBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.shadowRawBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, live.shadowBuf->GetGPUVirtualAddress());
    const uint32_t post[8] = { live.width, live.height, g_nrd.sigmaPitch, live.shadowPitch, live.debugView == 1 ? 1u : 0u, 0, 0, 0 };
    list->SetComputeRoot32BitConstants(3, 8, post, 0);
    list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
    Transition(list, live.shadowRawBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, g_nrd.sigmaBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    g_nrd.timeSum += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
    return true;
}
#else
void NrdDestroy() {}
#endif

// URTI (Universal Ray Tracing Injector), add-on ReShade, version 0 : capture de la géométrie d'un jeu
// DirectX 12 sans toucher à son rendu, et test de couverture.
//
// Pour chaque pipeline graphique du jeu, un pipeline « ombre » : même vertex shader, même signature
// racine, sortie de flux (stream output) de SV_Position, sans rastérisation ni pixel shader. Pendant
// la frame capturée, chaque dessin est rejoué une fois avec ce pipeline : on obtient les triangles
// tels que le jeu les a transformés (animation, instanciation, compression comprises), en espace de
// découpage. Test : ces triangles, rastérisés à nouveau, doivent reproduire la profondeur du jeu
// (aucune caméra nécessaire).
//
// D3D12 exige ALLOW_STREAM_OUTPUT dans la signature racine : on l'ajoute à celles du jeu à leur
// création (ID3D12Device::CreateRootSignature).
//
// Variables d'environnement : URTI_OUT (dossier du journal, sinon celui de l'exécutable),
// URTI_CAPTURE_FRAME (frame capturée, 60 par défaut).

#include <imgui.h>
#include <reshade.hpp>

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace reshade::api;

extern "C" __declspec(dllexport) const char* NAME = "URTI";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Universal Ray Tracing Injector : capture de la géométrie (version 0).";

namespace
{
// ------------------------------------------------------------------------------------------------
// journal
// ------------------------------------------------------------------------------------------------

std::string g_logPath;

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

void PatchDevice(ID3D12Device* device)
{
    void** vtable = *reinterpret_cast<void***>(device);
    constexpr int kCreateRootSignature = 16; // IUnknown (3) + ID3D12Object (4) + ID3D12Device jusqu'à CreateRootSignature
    if (vtable[kCreateRootSignature] == reinterpret_cast<void*>(&HookCreateRootSignature))
        return;
    DWORD old = 0;
    VirtualProtect(&vtable[kCreateRootSignature], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    g_createRootSignature = reinterpret_cast<PFN_CreateRootSignature>(vtable[kCreateRootSignature]);
    vtable[kCreateRootSignature] = reinterpret_cast<void*>(&HookCreateRootSignature);
    VirtualProtect(&vtable[kCreateRootSignature], sizeof(void*), old, &old);
    Log("URTI : CreateRootSignature intercepté");
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
    bool usable = false;           // vertex shader seul (pas de géométrie / tessellation / mesh)
    ComPtr<ID3D12PipelineState> shadow;
    bool shadowFailed = false;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, PipelineInfo> g_pipelines;

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
            info.semantics.reserve(s.count);
            for (uint32_t k = 0; k < s.count; k++)
                info.semantics.push_back(e[k].semantic ? e[k].semantic : "");
            for (uint32_t k = 0; k < s.count; k++)
            {
                D3D12_INPUT_ELEMENT_DESC d = {};
                d.SemanticName = info.semantics[k].c_str();
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
    info.rootSignature = reinterpret_cast<ID3D12RootSignature*>(layout.handle);
    info.usable = !other && info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && info.rootSignature;
    // les noms de sémantique pointent dans info.semantics : à refaire après le déplacement
    std::lock_guard<std::mutex> lock(g_mutex);
    PipelineInfo& stored = g_pipelines[pipe.handle] = std::move(info);
    for (size_t k = 0; k < stored.layout.size(); k++)
        stored.layout[k].SemanticName = stored.semantics[k].c_str();
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
        Log("URTI : pipeline ombre refusé (vertex shader de %zu octets)", info.vs.size());
    }
    return info.shadow.Get();
}

// ------------------------------------------------------------------------------------------------
// capture
// ------------------------------------------------------------------------------------------------

constexpr uint64_t kSoBytes = 128ull << 20; // 8 M sommets
constexpr uint32_t kMaxDraws = 16384;

struct DrawRecord
{
    uint64_t offset = 0, maxBytes = 0;
    uint64_t pipeline = 0;
    uint64_t depth = 0;   // ressource de profondeur liée au dessin
};

struct CommandListState
{
    uint64_t pipeline = 0;
    uint64_t depth = 0;
};

struct Capture
{
    ID3D12Device* device = nullptr;
    ComPtr<ID3D12Resource> so, counters, zeros, soReadback, counterReadback, depthReadback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;

    uint64_t frame = 0, captureFrame = 60;
    bool capturing = false;
    std::atomic<uint64_t> soUsed{ 0 };
    std::atomic<uint32_t> drawCount{ 0 };
    std::vector<DrawRecord> draws;
    std::atomic<uint32_t> framesDraws{ 0 }, skippedDraws{ 0 };
    std::unordered_map<uint64_t, CommandListState> lists;
    std::unordered_map<uint64_t, uint32_t> depthStates; // dernier état connu (barrières)
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

void OnInitDevice(device* dev)
{
    if (dev->get_api() != device_api::d3d12)
        return;
    ID3D12Device* device = reinterpret_cast<ID3D12Device*>(dev->get_native());
    PatchDevice(device);
    g.device = device;
    g.so = MakeBuffer(device, kSoBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
    g.counters = MakeBuffer(device, kMaxDraws * 8ull, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
    g.zeros = MakeBuffer(device, kMaxDraws * 8ull, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (g.zeros)
    {
        void* p = nullptr;
        if (SUCCEEDED(g.zeros->Map(0, nullptr, &p)))
        {
            std::memset(p, 0, kMaxDraws * 8ull);
            g.zeros->Unmap(0, nullptr);
        }
    }
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.list));
    if (g.list)
        g.list->Close();
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.draws.resize(kMaxDraws);
    Log("URTI : appareil D3D12 prêt (capture à la frame %llu)", (unsigned long long)g.captureFrame);
}

void OnBindPipeline(command_list* cmd, pipeline_stage stages, pipeline pipe)
{
    if ((stages & pipeline_stage::vertex_shader) == 0 && stages != pipeline_stage::all)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.lists[(uint64_t)cmd].pipeline = pipe.handle;
}

void OnBindRenderTargets(command_list* cmd, uint32_t, const resource_view*, resource_view dsv)
{
    const uint64_t depth = dsv.handle ? cmd->get_device()->get_resource_from_view(dsv).handle : 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g.lists[(uint64_t)cmd].depth = depth;
}

void OnBarrier(command_list*, uint32_t count, const resource* resources, const resource_usage*, const resource_usage* newStates)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < count; i++)
        if (g.depthStates.count(resources[i].handle) || (static_cast<uint32_t>(newStates[i]) & 0x30))
            g.depthStates[resources[i].handle] = static_cast<uint32_t>(newStates[i]);
}

// rejoue le dessin avec le pipeline ombre ; renvoie false (le dessin du jeu a lieu ensuite)
bool CaptureDraw(command_list* cmd, uint64_t verticesOut, const std::function<void(ID3D12GraphicsCommandList*)>& issue)
{
    if (!g.capturing)
        return false;
    g.framesDraws++;
    CommandListState state;
    PipelineInfo* info = nullptr;
    ID3D12PipelineState* shadow = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        state = g.lists[(uint64_t)cmd];
        auto it = g_pipelines.find(state.pipeline);
        if (it != g_pipelines.end() && it->second.usable)
        {
            info = &it->second;
            shadow = GetShadow(g.device, *info);
        }
    }
    const uint64_t bytes = verticesOut * 16;
    if (!shadow || bytes == 0)
    {
        g.skippedDraws++;
        return false;
    }
    const uint32_t index = g.drawCount++;
    const uint64_t offset = g.soUsed.fetch_add(bytes);
    if (index >= kMaxDraws || offset + bytes > kSoBytes)
    {
        g.skippedDraws++;
        return false;
    }
    g.draws[index] = { offset, bytes, state.pipeline, state.depth };

    ID3D12GraphicsCommandList* list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    D3D12_STREAM_OUTPUT_BUFFER_VIEW view = { g.so->GetGPUVirtualAddress() + offset, bytes, g.counters->GetGPUVirtualAddress() + index * 8ull };
    list->SetPipelineState(shadow);
    list->SOSetTargets(0, 1, &view);
    issue(list);
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW none = {};
    list->SOSetTargets(0, 1, &none);
    list->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(state.pipeline));
    return false;
}

bool OnDraw(command_list* cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    return CaptureDraw(cmd, 3ull * vertexCount * instanceCount, [&](ID3D12GraphicsCommandList* list) {
        list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    });
}

bool OnDrawIndexed(command_list* cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    return CaptureDraw(cmd, 3ull * indexCount * instanceCount, [&](ID3D12GraphicsCommandList* list) {
        list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    });
}

bool OnDrawIndirect(command_list*, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
{
    if (g.capturing && type != indirect_command::dispatch)
    {
        g.framesDraws++;
        g.skippedDraws++; // version 0 : dessins indirects comptés, pas encore capturés
    }
    return false;
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

void Submit(ID3D12CommandQueue* queue)
{
    g.list->Close();
    ID3D12CommandList* lists[] = { g.list.Get() };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g.fence.Get(), ++g.fenceValue);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.fence->SetEventOnCompletion(g.fenceValue, e);
    WaitForSingleObject(e, 10000);
    CloseHandle(e);
}

// ------------------------------------------------------------------------------------------------
// analyse : couverture de la profondeur du jeu par la géométrie capturée
// ------------------------------------------------------------------------------------------------

void Analyse(const std::vector<float>& depth, uint32_t width, uint32_t height, uint64_t depthResource,
             const uint8_t* so, const uint32_t* counters, uint32_t drawCount)
{
    // valeur d'effacement : la plus fréquente sur le bord de l'image (ciel)
    const float corner = depth[0];
    const bool reversed = corner < 0.5f;
    const uint32_t step = 2; // grille d'évaluation : un pixel sur deux
    const uint32_t w = width / step, h = height / step;
    std::vector<float> z(size_t(w) * h, reversed ? 0.0f : 1.0f);
    uint64_t triangles = 0, clipped = 0, drawsOnDepth = 0;

    for (uint32_t d = 0; d < drawCount && d < kMaxDraws; d++)
    {
        const DrawRecord& r = g.draws[d];
        if (r.depth != depthResource)
            continue;
        drawsOnDepth++;
        const uint64_t bytes = std::min<uint64_t>(counters[d * 2], r.maxBytes);
        const float* v = reinterpret_cast<const float*>(so + r.offset);
        const uint64_t verts = bytes / 16;
        for (uint64_t t = 0; t + 2 < verts; t += 3)
        {
            const float* a = v + 4 * t;
            const float* b = a + 4;
            const float* c = b + 4;
            if (a[3] <= 1e-5f || b[3] <= 1e-5f || c[3] <= 1e-5f)
            {
                clipped++; // version 0 : triangles qui traversent le plan proche ignorés
                continue;
            }
            triangles++;
            float sx[3], sy[3], sz[3];
            const float* p[3] = { a, b, c };
            for (int k = 0; k < 3; k++)
            {
                sx[k] = (p[k][0] / p[k][3] * 0.5f + 0.5f) * w;
                sy[k] = (0.5f - p[k][1] / p[k][3] * 0.5f) * h;
                sz[k] = p[k][2] / p[k][3];
            }
            const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::fabs(area) < 1e-12f)
                continue;
            const int x0 = std::max(0, (int)std::floor(std::min({ sx[0], sx[1], sx[2] })));
            const int x1 = std::min((int)w - 1, (int)std::ceil(std::max({ sx[0], sx[1], sx[2] })));
            const int y0 = std::max(0, (int)std::floor(std::min({ sy[0], sy[1], sy[2] })));
            const int y1 = std::min((int)h - 1, (int)std::ceil(std::max({ sy[0], sy[1], sy[2] })));
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

    uint64_t valid = 0, covered = 0, extra = 0;
    double error = 0;
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            const float game = depth[size_t(y * step + step / 2) * width + x * step + step / 2];
            const float ours = z[size_t(y) * w + x];
            const bool gameHas = game != corner;
            const bool oursHas = ours != (reversed ? 0.0f : 1.0f);
            if (gameHas)
            {
                valid++;
                const float diff = std::fabs(ours - game);
                if (oursHas && diff <= 1e-4f + 1e-3f * std::fabs(game))
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

void OnPresent(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*)
{
    if (!g.device || !g.list)
        return;
    ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());

    if (g.frame + 1 == g.captureFrame)
    {
        // compteurs du flux remis à zéro avant la frame capturée
        g.allocator->Reset();
        g.list->Reset(g.allocator.Get(), nullptr);
        Transition(g.list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyBufferRegion(g.counters.Get(), 0, g.zeros.Get(), 0, kMaxDraws * 8ull);
        Transition(g.list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        Submit(q);
        g.soUsed = 0;
        g.drawCount = 0;
        g.framesDraws = 0;
        g.skippedDraws = 0;
        g.capturing = true;
    }
    else if (g.capturing)
    {
        g.capturing = false;
        const uint32_t drawCount = std::min<uint32_t>(g.drawCount, kMaxDraws);
        const uint64_t used = std::min<uint64_t>(g.soUsed, kSoBytes);

        // profondeur principale : celle du plus grand nombre de dessins capturés
        std::unordered_map<uint64_t, uint32_t> votes;
        for (uint32_t d = 0; d < drawCount; d++)
            if (g.draws[d].depth)
                votes[g.draws[d].depth]++;
        uint64_t depthHandle = 0;
        uint32_t best = 0;
        for (auto& v : votes)
            if (v.second > best)
                best = v.second, depthHandle = v.first;

        Log("URTI : frame %llu : %u dessins, %u capturés, %u ignorés, %.1f Mo de sommets ; %d signatures racines modifiées",
            (unsigned long long)g.frame, (unsigned)g.framesDraws, drawCount, (unsigned)g.skippedDraws, used / 1048576.0, g_rootSignaturesPatched.load());

        g.soReadback = MakeBuffer(g.device, std::max<uint64_t>(used, 16), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        g.counterReadback = MakeBuffer(g.device, kMaxDraws * 8ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource* depth = reinterpret_cast<ID3D12Resource*>(depthHandle);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        D3D12_RESOURCE_DESC depthDesc = {};
        if (depth)
        {
            depthDesc = depth->GetDesc();
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&depthDesc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
            g.depthReadback = MakeBuffer(g.device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }

        g.allocator->Reset();
        g.list->Reset(g.allocator.Get(), nullptr);
        Transition(g.list.Get(), g.so.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(g.list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (used)
            g.list->CopyBufferRegion(g.soReadback.Get(), 0, g.so.Get(), 0, used);
        g.list->CopyBufferRegion(g.counterReadback.Get(), 0, g.counters.Get(), 0, kMaxDraws * 8ull);
        Transition(g.list.Get(), g.so.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
        Transition(g.list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
        D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        if (depth && g.depthReadback)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g.depthStates.find(depthHandle);
                if (it != g.depthStates.end())
                    depthState = static_cast<D3D12_RESOURCE_STATES>(it->second);
            }
            Transition(g.list.Get(), depth, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = g.depthReadback.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprint;
            src.pResource = depth;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(g.list.Get(), depth, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);
        }
        Submit(q);

        const uint8_t* soData = nullptr;
        const uint32_t* counterData = nullptr;
        const uint8_t* depthData = nullptr;
        g.soReadback->Map(0, nullptr, (void**)&soData);
        g.counterReadback->Map(0, nullptr, (void**)&counterData);
        if (depth && g.depthReadback && SUCCEEDED(g.depthReadback->Map(0, nullptr, (void**)&depthData)) && soData && counterData)
        {
            const uint32_t width = (uint32_t)depthDesc.Width, height = depthDesc.Height;
            std::vector<float> values(size_t(width) * height);
            const bool d32 = footprint.Footprint.Format == DXGI_FORMAT_R32_TYPELESS || footprint.Footprint.Format == DXGI_FORMAT_D32_FLOAT ||
                             footprint.Footprint.Format == DXGI_FORMAT_R32_FLOAT;
            for (uint32_t y = 0; y < height; y++)
            {
                const uint8_t* row = depthData + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch;
                for (uint32_t x = 0; x < width; x++)
                {
                    if (d32)
                        values[size_t(y) * width + x] = reinterpret_cast<const float*>(row)[x];
                    else // D24S8 : 24 bits de profondeur
                        values[size_t(y) * width + x] = (reinterpret_cast<const uint32_t*>(row)[x] & 0xFFFFFF) / 16777215.0f;
                }
            }
            Analyse(values, width, height, depthHandle, soData, counterData, drawCount);
            g.depthReadback->Unmap(0, nullptr);
        }
        else
            Log("URTI : pas de profondeur à comparer (format %d)", (int)depthDesc.Format);
        if (soData)
            g.soReadback->Unmap(0, nullptr);
        if (counterData)
            g.counterReadback->Unmap(0, nullptr);
    }
    g.frame++;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        if (!reshade::register_addon(module))
            return FALSE;
        char path[MAX_PATH] = {};
        if (const char* out = std::getenv("URTI_OUT"))
            g_logPath = std::string(out) + "\\urti_log.txt";
        else
        {
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            std::string exe = path;
            g_logPath = exe.substr(0, exe.find_last_of("\\/")) + "\\urti_log.txt";
        }
        if (const char* f = std::getenv("URTI_CAPTURE_FRAME"))
            g.captureFrame = std::max(2, std::atoi(f));
        reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
        reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
        reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
        reshade::register_event<reshade::addon_event::barrier>(OnBarrier);
        reshade::register_event<reshade::addon_event::draw>(OnDraw);
        reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(OnDrawIndirect);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        break;
    }
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}

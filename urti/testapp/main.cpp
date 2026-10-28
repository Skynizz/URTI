// URTI : application de test DirectX 12 en rastérisation, à géométrie connue (banc de l'injecteur).
//
// Scène : sol, six cubes fixes, un cube qui tourne, un cube déformé par le vertex shader, seize cubes
// instanciés. Signature racine créée sans ALLOW_STREAM_OUTPUT (comme un jeu) : c'est à l'injecteur
// de l'ajouter. Fenêtre jamais montrée sauf URTI_SHOW=1 ; URTI_FRAMES frames puis sortie.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT kWidth = 1280, kHeight = 720, kBuffers = 2;

struct Mat4
{
    float m[16];
};

Mat4 Mul(const Mat4& a, const Mat4& b) // convention vecteur ligne : v * a * b
{
    Mat4 r = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}

Mat4 Identity()
{
    Mat4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Scale(float x, float y, float z)
{
    Mat4 r = Identity();
    r.m[0] = x, r.m[5] = y, r.m[10] = z;
    return r;
}

Mat4 Translate(float x, float y, float z)
{
    Mat4 r = Identity();
    r.m[12] = x, r.m[13] = y, r.m[14] = z;
    return r;
}

Mat4 RotateY(float a)
{
    Mat4 r = Identity();
    r.m[0] = std::cos(a), r.m[2] = -std::sin(a), r.m[8] = std::sin(a), r.m[10] = std::cos(a);
    return r;
}

Mat4 LookAtLH(const float eye[3], const float at[3])
{
    float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float l = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float& v : z) v /= l;
    float x[3] = { z[2], 0.0f, -z[0] }; // up = (0, 1, 0) : x = up × z
    l = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    x[0] /= l, x[2] /= l;
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    Mat4 r = Identity();
    for (int i = 0; i < 3; i++)
    {
        r.m[i * 4 + 0] = x[i];
        r.m[i * 4 + 1] = y[i];
        r.m[i * 4 + 2] = z[i];
    }
    r.m[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    r.m[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    r.m[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    return r;
}

Mat4 PerspectiveLH(float fovY, float aspect, float zn, float zf)
{
    const float h = 1.0f / std::tan(fovY * 0.5f), w = h / aspect;
    Mat4 r = {};
    r.m[0] = w, r.m[5] = h, r.m[10] = zf / (zf - zn), r.m[11] = 1.0f, r.m[14] = -zn * zf / (zf - zn);
    return r;
}

const char* kShaders = R"(
cbuffer Frame : register(b0) { row_major float4x4 viewProj; float4 time; };
cbuffer Object : register(b1) { row_major float4x4 world; float4 misc; };
struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL;
#ifdef INSTANCED
    float3 ofs : INSTOFS;
#endif
};
struct VSOut { float4 pos : SV_Position; float3 nrm : NORMAL; float3 col : COLOR; };
VSOut vs(VSIn i)
{
    VSOut o;
    float3 p = i.pos;
    if (misc.x > 0.0) // déformation dans le vertex shader (comme une animation sur GPU)
        p += i.nrm * 0.25 * sin(4.0 * p.y + 3.0 * time.x) * (p.y + 0.5);
    float4 w = mul(float4(p, 1.0), world);
#ifdef INSTANCED
    w.xyz += i.ofs;
#endif
    o.pos = mul(w, viewProj);
    o.nrm = normalize(mul(float4(i.nrm, 0.0), world).xyz);
    o.col = misc.yzw;
    return o;
}
float4 ps(VSOut i) : SV_Target
{
    const float3 L = normalize(float3(0.4, 1.0, -0.3));
    return float4(i.col * (0.15 + 0.85 * saturate(dot(normalize(i.nrm), L))), 1.0);
}
)";

struct Vertex
{
    float p[3], n[3];
};

void MakeCube(std::vector<Vertex>& v, std::vector<uint16_t>& idx)
{
    const float n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int f = 0; f < 6; f++)
    {
        const float* N = n[f];
        const float u[3] = { N[1] != 0 ? 1.0f : 0.0f, N[1] == 0 ? 1.0f : 0.0f, 0.0f };
        const float t[3] = { N[1] * u[2] - N[2] * u[1], N[2] * u[0] - N[0] * u[2], N[0] * u[1] - N[1] * u[0] };
        const uint16_t base = (uint16_t)v.size();
        for (int k = 0; k < 4; k++)
        {
            const float a = (k == 1 || k == 2) ? 0.5f : -0.5f, b = (k >= 2) ? 0.5f : -0.5f;
            Vertex x;
            for (int c = 0; c < 3; c++)
            {
                x.p[c] = 0.5f * N[c] + a * u[c] + b * t[c];
                x.n[c] = N[c];
            }
            v.push_back(x);
        }
        // sens des triangles indifférent : pas d'élimination des faces
        const uint16_t q[6] = { 0, 1, 2, 0, 2, 3 };
        for (uint16_t k : q)
            idx.push_back(base + k);
    }
}

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s : 0x%08lx\n", #x, (unsigned long)hr_); std::exit(1); } } while (0)

ComPtr<ID3D12Resource> UploadBuffer(ID3D12Device* device, const void* data, size_t size)
{
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r)));
    if (data)
    {
        void* p = nullptr;
        CHECK(r->Map(0, nullptr, &p));
        std::memcpy(p, data, size);
        r->Unmap(0, nullptr);
    }
    return r;
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* target, bool instanced)
{
    const D3D_SHADER_MACRO macros[] = { { "INSTANCED", "1" }, { nullptr, nullptr } };
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShaders, std::strlen(kShaders), "urti_testapp", instanced ? macros : macros + 1, nullptr, entry, target, 0, 0, &code, &errors)))
    {
        std::fprintf(stderr, "%s\n", errors ? (const char*)errors->GetBufferPointer() : "erreur de compilation");
        std::exit(1);
    }
    return code;
}
} // namespace

int main()
{
    const char* frameEnv = std::getenv("URTI_FRAMES");
    const int frames = frameEnv ? std::atoi(frameEnv) : 120;
    const char* showEnv = std::getenv("URTI_SHOW");
    const bool show = showEnv && std::atoi(showEnv) != 0;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"UrtiTestApp";
    RegisterClassW(&wc);
    RECT rect = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"URTI test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (show)
        ShowWindow(hwnd, SW_SHOWNOACTIVATE); // jamais par défaut : l'écran sert à autre chose

    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D12Device> device;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = kWidth;
    scDesc.Height = kHeight;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = kBuffers;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    CHECK(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scDesc, nullptr, nullptr, &sc1));
    ComPtr<IDXGISwapChain3> swapchain;
    CHECK(sc1.As(&swapchain));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBuffers };
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    CHECK(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)));
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
    CHECK(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)));
    const UINT rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> backBuffers[kBuffers];
    for (UINT i = 0; i < kBuffers; i++)
    {
        CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * rtvStep;
        device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, h);
    }

    ComPtr<ID3D12Resource> depth;
    {
        D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)));
        device->CreateDepthStencilView(depth.Get(), nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // signature racine sans ALLOW_STREAM_OUTPUT, comme dans un jeu
    ComPtr<ID3D12RootSignature> rootSig;
    {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 1;
        params[1].Constants.Num32BitValues = 20;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, errors;
        CHECK(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
        CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    }

    ComPtr<ID3D12PipelineState> pso[2];
    for (int inst = 0; inst < 2; inst++)
    {
        ComPtr<ID3DBlob> vs = Compile("vs", "vs_5_1", inst != 0), ps = Compile("ps", "ps_5_1", inst != 0);
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "INSTOFS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSig.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.InputLayout = { layout, inst ? 3u : 2u };
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[inst])));
    }

    std::vector<Vertex> verts;
    std::vector<uint16_t> idx;
    MakeCube(verts, idx);
    ComPtr<ID3D12Resource> vb = UploadBuffer(device.Get(), verts.data(), verts.size() * sizeof(Vertex));
    ComPtr<ID3D12Resource> ib = UploadBuffer(device.Get(), idx.data(), idx.size() * sizeof(uint16_t));
    std::vector<float> offsets;
    for (int i = 0; i < 16; i++)
    {
        offsets.push_back(-6.0f + 1.5f * (i % 4));
        offsets.push_back(0.4f);
        offsets.push_back(4.0f + 1.5f * (i / 4));
    }
    ComPtr<ID3D12Resource> instances = UploadBuffer(device.Get(), offsets.data(), offsets.size() * sizeof(float));
    ComPtr<ID3D12Resource> frameCb = UploadBuffer(device.Get(), nullptr, 256 * kBuffers);
    uint8_t* frameCbData = nullptr;
    CHECK(frameCb->Map(0, nullptr, (void**)&frameCbData));

    const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress(), (UINT)(verts.size() * sizeof(Vertex)), sizeof(Vertex) };
    const D3D12_VERTEX_BUFFER_VIEW ivbv = { instances->GetGPUVirtualAddress(), (UINT)(offsets.size() * sizeof(float)), 12 };
    const D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGPUVirtualAddress(), (UINT)(idx.size() * sizeof(uint16_t)), DXGI_FORMAT_R16_UINT };

    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> cmd;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
    CHECK(cmd->Close());
    ComPtr<ID3D12Fence> fence;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0;

    const float eye[3] = { 7.0f, 6.0f, -11.0f }, at[3] = { 0.0f, 0.5f, 1.0f };
    const Mat4 viewProj = Mul(LookAtLH(eye, at), PerspectiveLH(1.05f, float(kWidth) / float(kHeight), 0.1f, 200.0f));

    struct Object
    {
        Mat4 world;
        float misc[4];
    };
    for (int frame = 0; frame < frames; frame++)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);

        const float t = frame / 60.0f;
        const UINT bb = swapchain->GetCurrentBackBufferIndex();
        uint8_t* cb = frameCbData + 256 * bb;
        std::memcpy(cb, viewProj.m, sizeof(viewProj.m));
        const float time[4] = { t, 0, 0, 0 };
        std::memcpy(cb + 64, time, sizeof(time));

        CHECK(allocator->Reset());
        CHECK(cmd->Reset(allocator.Get(), pso[0].Get()));
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffers[bb].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += bb * rtvStep;
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        const float sky[4] = { 0.45f, 0.6f, 0.85f, 1.0f };
        cmd->ClearRenderTargetView(rtv, sky, 0, nullptr);
        cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const D3D12_VIEWPORT vp = { 0, 0, float(kWidth), float(kHeight), 0, 1 };
        const D3D12_RECT sr = { 0, 0, (LONG)kWidth, (LONG)kHeight };
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sr);
        cmd->SetGraphicsRootSignature(rootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(0, frameCb->GetGPUVirtualAddress() + 256 * bb);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetIndexBuffer(&ibv);
        cmd->IASetVertexBuffers(0, 1, &vbv);

        auto draw = [&](const Object& o) {
            cmd->SetGraphicsRoot32BitConstants(1, 20, &o, 0);
            cmd->DrawIndexedInstanced((UINT)idx.size(), 1, 0, 0, 0);
        };
        draw({ Mul(Scale(24.0f, 0.2f, 24.0f), Translate(0.0f, -0.1f, 2.0f)), { 0, 0.55f, 0.5f, 0.45f } }); // sol
        for (int i = 0; i < 6; i++)
        {
            const float h = 0.6f + 0.4f * i;
            draw({ Mul(Scale(1.0f, h, 1.0f), Translate(-5.0f + 2.0f * i, h * 0.5f, -1.0f)), { 0, 0.8f, 0.3f + 0.1f * i, 0.25f } });
        }
        draw({ Mul(Mul(Scale(1.5f, 1.5f, 1.5f), RotateY(t)), Translate(3.5f, 0.75f, 2.0f)), { 0, 0.9f, 0.85f, 0.2f } }); // tourne
        draw({ Mul(Scale(1.2f, 2.0f, 1.2f), Translate(-2.0f, 1.0f, 2.0f)), { 1, 0.3f, 0.6f, 0.9f } });                  // déformé

        cmd->SetPipelineState(pso[1].Get()); // instanciés
        const D3D12_VERTEX_BUFFER_VIEW both[2] = { vbv, ivbv };
        cmd->IASetVertexBuffers(0, 2, both);
        const Object inst = { Scale(0.8f, 0.8f, 0.8f), { 0, 0.4f, 0.8f, 0.4f } };
        cmd->SetGraphicsRoot32BitConstants(1, 20, &inst, 0);
        cmd->DrawIndexedInstanced((UINT)idx.size(), 16, 0, 0, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd->ResourceBarrier(1, &barrier);
        CHECK(cmd->Close());
        ID3D12CommandList* lists[] = { cmd.Get() };
        queue->ExecuteCommandLists(1, lists);
        CHECK(swapchain->Present(0, 0));
        CHECK(queue->Signal(fence.Get(), ++fenceValue));
        CHECK(fence->SetEventOnCompletion(fenceValue, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    std::printf("urti_testapp : %d frames\n", frames);
    return 0;
}

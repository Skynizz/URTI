// URTI: ray-traced sun shadows on a capture (.urti), on the GPU (DXR, inline queries).
//
// The captured geometry goes from clip space to camera space (linear: x / P00, y / P11, w),
// becomes an acceleration structure; each game pixel is rebuilt from its depth
// (clip depth = A + B / w, A and B fitted on the captured vertices) and casts a ray toward the
// sun. Output: shadow mask as PFM (1 lit, 0 in shadow), at the depth's size.
//
//   urti_rtshadow capture.urti fov_degrees sun_x sun_y sun_z output.pfm [bias]

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>

#include "rtshadow_cs.h" // g_main: compiled shader (dxc)

using Microsoft::WRL::ComPtr;

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s : 0x%08lx\n", #x, (unsigned long)hr_); std::exit(1); } } while (0)

namespace
{
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav = false)
{
    D3D12_HEAP_PROPERTIES heap = { type };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size ? size : 16;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

ComPtr<ID3D12Resource> Upload(ID3D12Device* device, const void* data, uint64_t size)
{
    ComPtr<ID3D12Resource> r = Buffer(device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr;
    CHECK(r->Map(0, nullptr, &p));
    std::memcpy(p, data, size);
    r->Unmap(0, nullptr);
    return r;
}

void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    list->ResourceBarrier(1, &b);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 7)
    {
        std::fprintf(stderr, "usage: urti_rtshadow capture.urti fov_degrees sx sy sz output.pfm [bias]\n");
        return 1;
    }
    const float fov = float(std::atof(argv[2])) * 3.14159265f / 180.0f;
    float sun[3] = { float(std::atof(argv[3])), float(std::atof(argv[4])), float(std::atof(argv[5])) };
    const float bias = argc > 7 ? float(std::atof(argv[7])) : 0.003f;
    {
        const float l = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        for (float& v : sun) v /= l;
    }

    // --- reading the capture (version 2)
    FILE* f = std::fopen(argv[1], "rb");
    if (!f)
        return 1;
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> data(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    const uint32_t* h = reinterpret_cast<const uint32_t*>(data.data());
    if (h[0] != 0x49545255u || h[1] < 2)
    {
        std::fprintf(stderr, "URTI capture version 2 expected\n");
        return 1;
    }
    const uint32_t drawCount = h[2], mainSlot = h[3], width = h[4], height = h[5], slots = h[11];
    size_t off = 48;
    std::vector<uint64_t> slotBytes(slots);
    for (uint32_t s = 0; s < slots; s++)
    {
        const uint32_t* info = reinterpret_cast<const uint32_t*>(data.data() + off);
        slotBytes[s] = info[2] ? (uint64_t(info[3]) | (uint64_t(info[4]) << 32)) : 0;
        off += 24;
    }
    // draws of the last frame on the main depth: the camera may have moved between two frames
    struct Range { uint64_t start, end; };
    std::vector<Range> ranges;
    uint32_t lastFrame = 0;
    for (uint32_t d = 0; d < drawCount; d++)
    {
        const uint32_t* r = reinterpret_cast<const uint32_t*>(data.data() + off + size_t(d) * 56);
        if (r[0] == mainSlot)
            lastFrame = std::max(lastFrame, r[1]);
    }
    for (uint32_t d = 0; d < drawCount; d++)
    {
        const uint32_t* r = reinterpret_cast<const uint32_t*>(data.data() + off + size_t(d) * 56);
        const uint64_t* m = reinterpret_cast<const uint64_t*>(data.data() + off + size_t(d) * 56 + 40);
        if (r[0] == mainSlot && r[1] == lastFrame && m[1] > m[0])
            ranges.push_back({ m[0] / 16, m[1] / 16 });
    }
    off += size_t(drawCount) * 56;
    const float* clip = nullptr;
    for (uint32_t s = 0; s < slots; s++)
    {
        if (s == mainSlot)
            clip = reinterpret_cast<const float*>(data.data() + off);
        off += size_t(slotBytes[s]);
    }
    const float* depth = reinterpret_cast<const float*>(data.data() + off);
    const float clearDepth = h[10] ? 0.0f : 1.0f; // reversed depth: the sky is at 0
    uint64_t vertexCount = 0;
    for (const Range& r : ranges)
        vertexCount += (r.end - r.start) / 3 * 3;

    // --- camera space (linear in clip space) and depth law
    const float aspect = float(width) / float(height);
    const float p11 = 1.0f / std::tan(fov * 0.5f), p00 = p11 / aspect;
    std::vector<float> view(size_t(vertexCount) * 3);
    double sw = 0, sz = 0, sww = 0, swz = 0, n = 0;
    uint64_t o = 0;
    for (const Range& r : ranges)
        for (uint64_t i = r.start; i < r.start + (r.end - r.start) / 3 * 3; i++, o++)
        {
            const float* c = clip + i * 4;
            view[o * 3 + 0] = c[0] / p00, view[o * 3 + 1] = c[1] / p11, view[o * 3 + 2] = c[3];
            if (c[3] > 1e-3f && (o % 7) == 0)
                sw += c[3], sz += c[2], sww += double(c[3]) * c[3], swz += double(c[3]) * c[2], n += 1;
        }
    // clip z = A w + B (perspective projection): depth = A + B / w
    const double A = (n * swz - sw * sz) / (n * sww - sw * sw), B = (sz - A * sw) / n;
    std::printf("%llu vertices; depth = %.6f + %.6f / w\n", (unsigned long long)vertexCount, A, B);
    std::vector<float> positions(size_t(width) * height * 4, 0.0f);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            const float d = depth[size_t(y) * width + x];
            if (d == clearDepth)
                continue; // sky
            const double w = B / (double(d) - A);
            if (!(w > 0) || !(w < 1e7))
                continue; // sky
            const float nx = (x + 0.5f) / width * 2.0f - 1.0f, ny = 1.0f - (y + 0.5f) / height * 2.0f;
            float* p = &positions[(size_t(y) * width + x) * 4];
            p[0] = float(nx * w / p00), p[1] = float(ny * w / p11), p[2] = float(w), p[3] = 1.0f;
        }

    // --- GPU
    ComPtr<ID3D12Device5> device;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    {
        std::fprintf(stderr, "inline ray tracing (DXR 1.1) unavailable\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> alloc;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList4> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)));

    ComPtr<ID3D12Resource> vb = Upload(device.Get(), view.data(), view.size() * sizeof(float));
    ComPtr<ID3D12Resource> pos = Upload(device.Get(), positions.data(), positions.size() * sizeof(float));

    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer = { vb->GetGPUVirtualAddress(), 12 };
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = UINT(vertexCount / 3 * 3);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasIn = {};
    blasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasIn.NumDescs = 1;
    blasIn.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bi = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&blasIn, &bi);
    ComPtr<ID3D12Resource> blas = Buffer(device.Get(), bi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    ComPtr<ID3D12Resource> scratch = Buffer(device.Get(), bi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs = blasIn;
    bd.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    UavBarrier(list.Get(), blas.Get());

    D3D12_RAYTRACING_INSTANCE_DESC inst = {};
    inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
    inst.InstanceMask = 0xFF;
    inst.AccelerationStructure = blas->GetGPUVirtualAddress();
    ComPtr<ID3D12Resource> instances = Upload(device.Get(), &inst, sizeof(inst));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn = {};
    tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasIn.NumDescs = 1;
    tlasIn.InstanceDescs = instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ti = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &ti);
    ComPtr<ID3D12Resource> tlas = Buffer(device.Get(), ti.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    ComPtr<ID3D12Resource> tscratch = Buffer(device.Get(), ti.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs = tlasIn;
    td.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    td.ScratchAccelerationStructureData = tscratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    UavBarrier(list.Get(), tlas.Get());

    // compute shader: SRV t0 (scene), SRV t1 (positions), UAV u0 (shadow), constants b0
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 1;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.Num32BitValues = 8;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> rsBlob, rsErr;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr));
    ComPtr<ID3D12RootSignature> rs;
    CHECK(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {};
    cpd.pRootSignature = rs.Get();
    cpd.CS = { g_main, sizeof(g_main) };
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso)));

    const uint32_t count = width * height;
    ComPtr<ID3D12Resource> out = Buffer(device.Get(), count * 4ull, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    ComPtr<ID3D12Resource> readback = Buffer(device.Get(), count * 4ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    struct { float sun[3]; float bias; uint32_t count; float maxDistance; float pad[2]; } constants = { { sun[0], sun[1], sun[2] }, bias, count, 1e5f, { 0, 0 } };
    list->SetComputeRootSignature(rs.Get());
    list->SetPipelineState(pso.Get());
    list->SetComputeRootShaderResourceView(0, tlas->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, pos->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstants(3, 8, &constants, 0);
    list->Dispatch((count + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyResource(readback.Get(), out.Get());
    CHECK(list->Close());
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    CHECK(queue->Signal(fence.Get(), 1));
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, e);
    WaitForSingleObject(e, INFINITE);

    const float* mask = nullptr;
    CHECK(readback->Map(0, nullptr, (void**)&mask));
    uint64_t lit = 0, valid = 0;
    for (uint32_t i = 0; i < count; i++)
        if (positions[i * 4 + 3] != 0)
            valid++, lit += mask[i] > 0.5f;
    if (FILE* o = std::fopen(argv[6], "wb"))
    {
        std::fprintf(o, "Pf\n%u %u\n-1.0\n", width, height);
        for (int y = int(height) - 1; y >= 0; y--)
            std::fwrite(mask + size_t(y) * width, 4, width, o);
        std::fclose(o);
    }
    std::printf("BLAS %.1f MB; %llu geometry pixels, %.1f %% lit by the sun\n", bi.ResultDataMaxSizeInBytes / 1048576.0,
                (unsigned long long)valid, valid ? 100.0 * lit / valid : 0.0);
    return 0;
}

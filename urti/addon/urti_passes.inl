// URTI: pass survey (included in urti_addon.cpp). F8 or the urti_passes file next to the add-on:
// during one game frame, every image it draws into (render targets) or writes in compute (UAV,
// seen through their state transitions) is recorded: size, format, bindings, draws, full-screen
// draws. At the end, a log sorted by order of appearance and a copy of each screen-sized candidate
// as an image (urti_pass_NNN.ppm). Used to find the game's sun shadow mask: an image
// with one or two channels, the size of the screen, written after the shadow map and before lighting.
// Nothing in the game's rendering is modified.

struct PassTarget
{
    uint32_t width = 0, height = 0, format = 0, order = 0;
    uint32_t binds = 0, draws = 0, fullscreen = 0, uavTransitions = 0;
    bool withSunDepth = false;
};

struct PassSurvey
{
    std::atomic<int> state{ 0 }; // 0 nothing, 1 armed, 2 survey in progress
    uint32_t presents = 0, order = 0, draws = 0, dispatchLike = 0;
    std::unordered_map<uint64_t, PassTarget> targets;
    std::unordered_map<uint64_t, std::vector<uint64_t>> current; // command list -> bound targets
    std::unordered_map<uint64_t, uint32_t> states;               // last known state of each resource
    // snapshots: copy of a screen-sized image when the game finishes writing it
    // (render target / UAV -> read transition), before its memory serves something else
    struct Snapshot { ComPtr<ID3D12Resource> copy; uint32_t order, format, width, height; };
    std::vector<Snapshot> snapshots;
} g_pass;
constexpr uint32_t kPassMaxSnapshots = 64;

PassTarget& PassTargetFor(uint64_t res)
{
    auto it = g_pass.targets.find(res);
    if (it != g_pass.targets.end())
        return it->second;
    PassTarget t;
    const D3D12_RESOURCE_DESC d = reinterpret_cast<ID3D12Resource*>(res)->GetDesc();
    t.width = uint32_t(d.Width), t.height = d.Height, t.format = uint32_t(d.Format), t.order = g_pass.order++;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        t.width = t.height = 0;
    return g_pass.targets.emplace(res, t).first->second;
}

void PassBind(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view dsv)
{
    if (g_pass.state != 2 || !OurDevice(cmd->get_device()))
        return;
    device* dev = cmd->get_device();
    const uint64_t depth = dsv.handle ? dev->get_resource_from_view(dsv).handle : 0;
    std::vector<uint64_t> bound;
    for (uint32_t i = 0; i < count && rtvs; i++)
        if (rtvs[i].handle)
            bound.push_back(dev->get_resource_from_view(rtvs[i]).handle);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint64_t r : bound)
    {
        if (!r)
            continue;
        PassTarget& t = PassTargetFor(r);
        t.binds++;
        t.withSunDepth |= depth && depth == g_liveSunDepth;
    }
    g_pass.current[cmd->get_native()] = std::move(bound);
}

void PassDraw(uint64_t list, bool fullscreen)
{
    if (g_pass.state != 2)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pass.draws++;
    auto it = g_pass.current.find(list);
    if (it == g_pass.current.end())
        return;
    for (uint64_t r : it->second)
    {
        PassTarget& t = g_pass.targets[r];
        t.draws++;
        t.fullscreen += fullscreen ? 1 : 0;
    }
}

// ReShade state -> D3D12 state (present and general have values specific to ReShade)
uint32_t PassD3D12State(uint32_t usage)
{
    if (usage & 0x80000000u) // present, general
        return D3D12_RESOURCE_STATE_COMMON;
    uint32_t s = usage & ~0x8000u;
    if (usage & 0x8000u) // constant_buffer
        s |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    return s;
}

bool PassDecode(uint32_t fmt, const uint8_t* p, float rgb[3], int& channels);

void PassBarrier(command_list* cmd, uint32_t count, const resource* resources, const resource_usage* oldStates, const resource_usage* newStates)
{
    if (g_pass.state != 2 || !OurDevice(cmd->get_device()))
        return;
    ID3D12GraphicsCommandList* list = Native(cmd);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < count; i++)
    {
        if (!resources[i].handle) // global or aliasing UAV barrier: no resource
            continue;
        const uint32_t s = PassD3D12State(static_cast<uint32_t>(newStates[i]));
        const uint32_t before = PassD3D12State(static_cast<uint32_t>(oldStates[i]));
        g_pass.states[resources[i].handle] = s;
        PassTarget& t = PassTargetFor(resources[i].handle);
        if (s & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            t.uavTransitions++;
        // end of writing of a screen-sized image: snapshot
        const bool written = (before & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) &&
                             (s & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) &&
                             !(s & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        const bool screenSized = t.width && g.screenW && t.width >= g.screenW / 4 && t.height >= g.screenH / 4 &&
                                 std::fabs(double(t.width) / t.height - double(g.screenW) / g.screenH) < 0.1;
        if (!written || !screenSized || g_pass.snapshots.size() >= kPassMaxSnapshots)
            continue;
        // two snapshots at most per frame: an image rewritten at every step (blur, accumulation) took every
        // slot before the G-buffer targets (seen in one game)
        if (std::count_if(g_pass.snapshots.begin(), g_pass.snapshots.end(), [&](const PassSurvey::Snapshot& n) { return n.order == t.order; }) >= 2)
            continue;
        ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(resources[i].handle);
        D3D12_RESOURCE_DESC d = res->GetDesc();
        float probe[3];
        int ch = 0;
        const uint8_t zero[16] = {};
        if (d.SampleDesc.Count > 1 || (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || !PassDecode(uint32_t(d.Format), zero, probe, ch))
            continue;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
        ComPtr<ID3D12Resource> copy;
        if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy))))
            continue;
        Transition(list, res, D3D12_RESOURCE_STATES(s), D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(copy.Get(), res);
        Transition(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(s));
        g_pass.snapshots.push_back({ copy, t.order, t.format, t.width, t.height });
    }
}

// a texel -> rgb (useful channels), false if the format is not read
bool PassDecode(uint32_t fmt, const uint8_t* p, float rgb[3], int& channels)
{
    auto half = [](uint16_t h) {
        const uint32_t e = (h >> 10) & 31, m = h & 1023;
        const float v = e ? std::ldexp(1.0f + m / 1024.0f, int(e) - 15) : std::ldexp(m / 1024.0f, -14);
        return (h & 0x8000) ? -v : v;
    };
    auto packedFloat = [](uint32_t bits, int mb) { // unsigned 11- or 10-bit float
        const uint32_t m = bits & ((1u << mb) - 1), e = bits >> mb;
        return e ? std::ldexp(1.0f + float(m) / (1 << mb), int(e) - 15) : std::ldexp(float(m) / (1 << mb), -14);
    };
    const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
    const float* f = reinterpret_cast<const float*>(p);
    const uint32_t u = *reinterpret_cast<const uint32_t*>(p);
    rgb[0] = rgb[1] = rgb[2] = 0;
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        channels = 3; for (int c = 0; c < 3; c++) rgb[c] = p[c] / 255.0f; return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        channels = 3; for (int c = 0; c < 3; c++) rgb[c] = p[2 - c] / 255.0f; return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        channels = 3; for (int c = 0; c < 3; c++) rgb[c] = ((u >> (10 * c)) & 1023) / 1023.0f; return true;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        channels = 3; rgb[0] = packedFloat(u & 2047, 6), rgb[1] = packedFloat((u >> 11) & 2047, 6), rgb[2] = packedFloat(u >> 22, 5); return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        channels = 3; for (int c = 0; c < 3; c++) rgb[c] = half(h[c]); return true;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
        channels = 2; rgb[0] = half(h[0]), rgb[1] = half(h[1]); return true;
    case DXGI_FORMAT_R16G16_UNORM:
        channels = 2; rgb[0] = h[0] / 65535.0f, rgb[1] = h[1] / 65535.0f; return true;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT:
        channels = 1; rgb[0] = half(h[0]); return true;
    case DXGI_FORMAT_R16_UNORM:
        channels = 1; rgb[0] = h[0] / 65535.0f; return true;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM:
        channels = 1; rgb[0] = p[0] / 255.0f; return true;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM:
        channels = 2; rgb[0] = p[0] / 255.0f, rgb[1] = p[1] / 255.0f; return true;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
        channels = 1; rgb[0] = f[0]; return true;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
        channels = 2; rgb[0] = f[0], rgb[1] = f[1]; return true;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        channels = 3; for (int c = 0; c < 3; c++) rgb[c] = f[c]; return true;
    default:
        return false;
    }
}

uint32_t PassTexelBytes(uint32_t fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM: return 2;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 4;
    }
}

// copy of the resource (subresource 0, half resolution) as an image: values brought to [0, 1] by the
// image's maximum (float formats), unused channels at zero
bool PassSave(ID3D12CommandQueue* queue, ID3D12Resource* res, D3D12_RESOURCE_STATES state, const std::string& path)
{
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    // no multisampling, no depth, no unknown format
    float probe[3];
    int probeChannels = 0;
    const uint8_t zero[16] = {};
    if (desc.SampleDesc.Count > 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || !PassDecode(uint32_t(desc.Format), zero, probe, probeChannels))
        return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> readback = MakeBuffer(g.device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback)
        return false;
    BeginList();
    Transition(g.list.Get(), res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(g.list.Get(), res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    Submit(queue);
    const uint8_t* data = nullptr;
    if (FAILED(readback->Map(0, nullptr, (void**)&data)))
        return false;
    const uint32_t w = uint32_t(desc.Width) / 2, h = desc.Height / 2, texel = PassTexelBytes(uint32_t(desc.Format));
    // raw texels next to it (same half resolution, every channel, alpha included): G-buffer study
    // (tools/reshade/urti_gbuffer.py)
    if (FILE* raw = std::fopen((path.substr(0, path.size() - 4) + ".raw").c_str(), "wb"))
    {
        std::fprintf(raw, "URTIRAW %u %u %u %u\n", uint32_t(desc.Format), w, h, texel);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                std::fwrite(data + fp.Offset + size_t(y * 2) * fp.Footprint.RowPitch + size_t(x * 2) * texel, 1, texel, raw);
        std::fclose(raw);
    }
    std::vector<float> img(size_t(w) * h * 3);
    float peak[3] = { 1e-6f, 1e-6f, 1e-6f };
    int channels = 1;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            float rgb[3];
            PassDecode(uint32_t(desc.Format), data + fp.Offset + size_t(y * 2) * fp.Footprint.RowPitch + size_t(x * 2) * texel, rgb, channels);
            for (int c = 0; c < 3; c++)
            {
                const float v = std::isfinite(rgb[c]) ? std::fabs(rgb[c]) : 0.0f;
                img[(size_t(y) * w + x) * 3 + c] = v;
                peak[c] = std::max(peak[c], v);
            }
        }
    readback->Unmap(0, nullptr);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
            {
                // one channel: grey; two: red and green
                const int sc = channels == 1 ? 0 : c;
                const float v = (channels == 2 && c == 2) ? 0.0f : img[(size_t(y) * w + x) * 3 + sc] / (peak[sc] > 1.0f ? peak[sc] : 1.0f);
                row[size_t(x) * 3 + c] = uint8_t(std::min(255.0f, 255.0f * std::pow(std::min(v, 1.0f), 1.0f / 2.2f)));
            }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// on every present: arming (F8, file), then end of the survey and writing
void PassPresent(ID3D12CommandQueue* queue)
{
    const std::string trigger = g_dir + "\\urti_passes";
    if (g_pass.state == 0 && ((GetAsyncKeyState(VK_F8) & 1) || (g.frame % 30 == 0 && GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES)))
    {
        DeleteFileA(trigger.c_str());
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pass.targets.clear(), g_pass.current.clear(), g_pass.states.clear(), g_pass.snapshots.clear();
        g_pass.presents = g_pass.order = g_pass.draws = 0;
        g_pass.state = 2;
        Log("URTI: pass survey started");
        return;
    }
    if (g_pass.state != 2)
        return;
    // a present with game draws (frame generation inserts some without)
    if (++g_pass.presents < 2 || (g_pass.draws < 50 && g_pass.presents < 12))
        return;
    g_pass.state = 0;
    std::vector<std::pair<uint64_t, PassTarget>> list(g_pass.targets.begin(), g_pass.targets.end());
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.order < b.second.order; });
    Log("URTI: pass survey: %u draws, %zu images (render targets or UAV)", g_pass.draws, list.size());
    uint32_t saved = 0;
    for (const auto& [res, t] : list)
    {
        if (!t.width || (!t.binds && !t.uavTransitions))
            continue;
        const bool screenSized = g.screenW && t.width >= g.screenW / 4 && t.height >= g.screenH / 4 &&
                                 std::fabs(double(t.width) / t.height - double(g.screenW) / g.screenH) < 0.1;
        std::string file;
        if (screenSized && saved < 64)
        {
            auto st = g_pass.states.find(res);
            if (st != g_pass.states.end())
            {
                char name[64];
                std::snprintf(name, sizeof(name), "urti_pass_%03u.ppm", t.order);
                if (PassSave(queue, reinterpret_cast<ID3D12Resource*>(res), D3D12_RESOURCE_STATES(st->second), g_dir + "\\" + name))
                    file = name, saved++;
            }
            else
                file = "(unknown state, not copied)";
        }
        Log("URTI: pass %03u: %ux%u format %u, %u bindings, %u draws of which %u full screen, %u UAV transitions%s %s", t.order, t.width,
            t.height, t.format, t.binds, t.draws, t.fullscreen, t.uavTransitions, t.withSunDepth ? ", with the shadow map" : "", file.c_str());
    }
    // snapshots taken at the end of the writing of each image (in the frame's order)
    uint32_t index = 0;
    for (const auto& snap : g_pass.snapshots)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "urti_snapshot_%02u_pass_%03u.ppm", index++, snap.order);
        const bool ok = PassSave(queue, snap.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, g_dir + "\\" + name);
        Log("URTI: snapshot %s: %ux%u format %u%s", name, snap.width, snap.height, snap.format, ok ? "" : " (not written)");
    }
    g_pass.snapshots.clear();
    // targets of the detected G-buffer (bound together with the screen depth)
    for (uint32_t k = 0; k < g_liveGbufferCount; k++)
        if (auto it = g_pass.targets.find(g_liveGbuffer[k]); g_liveGbuffer[k] && it != g_pass.targets.end())
            Log("URTI: G-buffer target %u = pass %03u (%ux%u format %u)", k, it->second.order, it->second.width, it->second.height, it->second.format);
    Log("URTI: pass survey done, %u images copied, %u snapshots", saved, index);
}

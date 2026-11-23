// Live URTI: convention of the game's motion vectors (the ones DLSS receives), found on
// its own. For 16 candidate conventions (unit: screen 0..1, NDC -1..1 or pixels; sign; vertical axis
// flipped or not), the previous frame's depth read at the indicated place must be this pixel's
// (still objects, small motions). Mean error per candidate, accumulated by the CPU.

ByteAddressBuffer depth : register(t0);     // this frame's depth
ByteAddressBuffer depthPrev : register(t1); // previous frame's depth
ByteAddressBuffer motion : register(t2);    // motion vectors (R16G16 floats)
ByteAddressBuffer sunBuf : register(t3);    // depth law (bytes 36 to 47)
RWByteAddressBuffer errors : register(u0);  // 16 × (sum of errors, count) as integers, + mean motion

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint motionPitch;
    uint depth24;
    uint pad0;
    uint pad1;
    uint pad2;
};

float ViewDepth(ByteAddressBuffer b, int2 t, float A, float B)
{
    t = clamp(t, int2(0, 0), int2(width - 1, height - 1));
    const uint raw = b.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    return B / (d - A);
}

float2 Candidate(uint k, float2 mv)
{
    // unit: 0 screen, 1 NDC (half), 2 pixels; then sign, then vertical flip
    const float2 size = float2(width, height);
    float2 m = (k & 3) == 0 ? mv : (k & 3) == 1 ? mv * 0.5 : (k & 3) == 2 ? mv / size : mv * 2.0;
    if (k & 4)
        m = -m;
    if (k & 8)
        m.y = -m.y;
    return m;
}

[numthreads(256, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    const float3 law = asfloat(sunBuf.Load3(36));
    if (law.z <= 0)
        return;
    // samples spread over the image (shifted 64 × 64 grid)
    const uint gx = id % 64, gy = id / 64;
    if (gy >= 64)
        return;
    const int2 t = int2((gx + 0.5) / 64.0 * width, (gy + 0.5) / 64.0 * height);
    const float w = ViewDepth(depth, t, law.x, law.y);
    if (!(w > 0 && w < 1e5))
        return;
    const uint raw = motion.Load(t.y * motionPitch + t.x * 4);
    const float2 mv = float2(f16tof32(raw & 0xFFFF), f16tof32(raw >> 16));
    if (!all(isfinite(mv)) || dot(mv, mv) < 1e-12)
        return;
    const float2 uv = (float2(t) + 0.5) / float2(width, height);
    for (uint k = 0; k < 16; k++)
    {
        const float2 p = uv + Candidate(k, mv);
        if (any(p < 0) || any(p > 1))
            continue;
        const float wp = ViewDepth(depthPrev, int2(p * float2(width, height)), law.x, law.y);
        if (!(wp > 0 && wp < 1e5))
            continue;
        const float e = min(abs(log(wp / w)), 1.0);
        uint dummy;
        errors.InterlockedAdd(k * 8, uint(e * 10000.0), dummy);
        errors.InterlockedAdd(k * 8 + 4, 1, dummy);
    }
    // mean motion (to judge only in motion): |mv| in millionths
    uint dummy2;
    errors.InterlockedAdd(128, uint(min(length(mv), 1.0) * 1e6), dummy2);
    errors.InterlockedAdd(132, 1, dummy2);
}

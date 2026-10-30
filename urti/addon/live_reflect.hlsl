// URTI en direct : reflets en ray tracing. Un rayon par pixel lisse (selon le matériau du G-buffer du
// jeu), réfléchi autour de la normale tirée de la profondeur, lancé dans la géométrie captée. Le point
// touché est reprojeté à l'écran : s'il y est visible (même profondeur), sa couleur sera lue dans
// l'image du jeu par la composition. Sortie par pixel : position à l'écran du point reflété (u, v),
// poids (Fresnel × lissé), 0 si rien d'utilisable (le reflet du jeu reste alors).

RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer depth : register(t1);    // profondeur du jeu (32 bits par texel, lignes alignées)
ByteAddressBuffer material : register(t2); // copie de l'image de matériau du G-buffer (32 bits par texel)
ByteAddressBuffer sunBuf : register(t3);   // octets 36 à 47 : loi de profondeur de cette image
RWByteAddressBuffer reflection : register(u0); // float4 par pixel : u, v, poids, lissé

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint materialPitch;
    float A;
    float B;
    float p00;
    float p11;
    float clearDepth;
    uint depth24;
    uint channel;      // canal du matériau qui porte le lissé (0 à 3)
    float smoothMin;   // en dessous : pas de reflet RT
    float bias;
    uint flags;        // 1 matériau lu, 2 inverser le canal (rugosité au lieu de lissé), 4 tout est lisse (contrôle)
    float f0;          // réflectance à incidence normale (diélectrique ~0,04)
    uint reflectPitch;
};

static float gA, gB, gClear;

float3 PointAt(int2 t)
{
    if (t.x < 0 || t.y < 0 || t.x >= int(width) || t.y >= int(height))
        return float3(0, 0, -1);
    const uint raw = depth.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float w = gB / (d - gA);
    if (d == gClear || !(w > 0 && w < 1e6))
        return float3(0, 0, -1);
    const float2 ndc = float2((t.x + 0.5) / width * 2 - 1, 1 - (t.y + 0.5) / height * 2);
    return float3(ndc.x * w / p00, ndc.y * w / p11, w);
}

float3 NormalAt(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(1, 0)), r = PointAt(t + int2(1, 0)), u = PointAt(t - int2(0, 1)), b = PointAt(t + int2(0, 1));
    const float3 dx = (r.z > 0 && (l.z <= 0 || abs(r.z - p.z) < abs(l.z - p.z))) ? r - p : p - l;
    const float3 dy = (b.z > 0 && (u.z <= 0 || abs(b.z - p.z) < abs(u.z - p.z))) ? b - p : p - u;
    float3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0 ? -n : n;
}

float Smoothness(int2 t)
{
    if (flags & 4) // contrôle : toutes les surfaces lisses
        return 1.0;
    if ((flags & 1) == 0)
        return 0;
    const uint raw = material.Load(t.y * materialPitch + t.x * 4);
    float s = float((raw >> (8 * channel)) & 255) / 255.0;
    return (flags & 2) ? 1.0 - s : s;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    // loi de profondeur mesurée dans cette image (live_depthfit), sinon celle des constantes
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.z > 0 ? law.x : A, gB = law.z > 0 ? law.y : B;
    gClear = gB > 0 ? 0.0 : 1.0; // profondeur inversée : le ciel à 0
    const float3 p = PointAt(int2(id));
    const float s = Smoothness(int2(id));
    float4 result = float4(0, 0, 0, s);
    if (p.z > 0 && s >= smoothMin)
    {
        const float3 n = NormalAt(int2(id), p);
        const float3 v = normalize(p);
        const float3 r = reflect(v, n);
        const float cosTheta = saturate(dot(-v, n));
        // Fresnel (Schlick) ; le lissé au-dessus du seuil module le poids
        const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
        const float weight = fresnel * saturate((s - smoothMin) / max(1e-3, 1.0 - smoothMin));
        RayDesc ray;
        ray.Origin = p + n * (bias * p.z);
        ray.Direction = r;
        ray.TMin = 0.0;
        ray.TMax = 1e5;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && r.z > -0.999)
        {
            const float3 h = ray.Origin + r * q.CommittedRayT();
            if (h.z > 1e-3)
            {
                const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
                const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                if (all(uv > 0.001) && all(uv < 0.999))
                {
                    // le point touché doit être celui que l'image montre à cet endroit (sinon il est caché)
                    const float3 seen = PointAt(int2(uv * float2(width, height)));
                    if (seen.z > 0 && abs(seen.z - h.z) < 0.03 * h.z + 0.05)
                    {
                        // fondu vers les bords de l'écran (couleur inconnue au-delà)
                        const float2 edge = saturate(min(uv, 1.0 - uv) * 12.0);
                        result = float4(uv, weight * edge.x * edge.y, s);
                    }
                }
            }
        }
    }
    reflection.Store4(id.y * reflectPitch + id.x * 16, asuint(result));
}

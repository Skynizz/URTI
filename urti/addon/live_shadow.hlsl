// URTI en direct : ombres du soleil en ray tracing (requêtes en ligne). Un fil par pixel de la
// profondeur du jeu : point reconstruit (profondeur = A + B / w), rayon vers le soleil.

RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer depth : register(t1);   // copie de la profondeur (32 bits par texel, lignes alignées)
ByteAddressBuffer sunBuf : register(t2);  // soleil de cette image (live_sunvote), espace caméra, cascades
ByteAddressBuffer atlas : register(t3);   // copie de la carte d'ombres du jeu (32 bits par texel, lignes alignées)
RWByteAddressBuffer shadow : register(u0); // 1 éclairé, 0 à l'ombre (float, lignes alignées)

cbuffer Constants : register(b0)
{
    float sunSign;     // +1 : la profondeur de la carte croît en s'éloignant du soleil
    float gameBias;    // tolérance du test de profondeur de la carte du jeu
    float sunRadius;   // rayon apparent du soleil (radians) : pénombre
    float bias;
    uint width;
    uint height;
    uint depthPitch;   // octets par ligne
    uint shadowPitch;
    float A;
    float B;
    float p00;
    float p11;
    float clearDepth;
    uint depth24;      // 1 : D24S8 (24 bits de profondeur)
    uint atlasPitch;
    uint flags;        // 1 carte du jeu lue, 2 carte en D24, 4 combiner avec l'ombre du jeu, 8 afficher l'ombre du jeu
};

// éclairement du point par le soleil selon la carte d'ombres du jeu (1 si aucune cascade ne le couvre)
float GameLit(float3 p)
{
    const uint count = min(sunBuf.Load(32), 8u);
    for (uint k = 0; k < count; k++)
    {
        const uint base = 48 + k * 64;
        const float4 m0 = asfloat(sunBuf.Load4(base)), m1 = asfloat(sunBuf.Load4(base + 16)), m2 = asfloat(sunBuf.Load4(base + 32));
        const float4 vp = asfloat(sunBuf.Load4(base + 48));
        const float M[12] = { m0.x, m0.y, m0.z, m0.w, m1.x, m1.y, m1.z, m1.w, m2.x, m2.y, m2.z, m2.w };
        float3 s;
        for (uint c = 0; c < 3; c++)
            s[c] = M[9 + c] + p.x * M[c] + p.y * M[3 + c] + p.z * M[6 + c];
        if (abs(s.x) > 0.98 || abs(s.y) > 0.98 || s.z <= 0 || s.z >= 1)
            continue;
        const float2 texel = vp.xy + float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5) * vp.zw;
        // éclairé seulement si tout le voisinage 5×5 l'est : l'ombre du jeu est élargie de deux texels,
        // ce qui absorbe le flou de ses bords (sinon un liseré d'ombre RT double chaque contour)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
            {
                const uint2 t = uint2(int2(texel) + int2(dx, dy));
                const uint raw = atlas.Load(t.y * atlasPitch + t.x * 4);
                const float stored = (flags & 2) ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
                if (!(sunSign > 0 ? s.z <= stored + gameBias : s.z >= stored - gameBias))
                    return 0.0;
            }
        return 1.0;
    }
    return 1.0;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint raw = depth.Load(id.y * depthPitch + id.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float3 sun = asfloat(sunBuf.Load3(0));
    float lit = 1.0;
    const float w = B / (d - A);
    if (d != clearDepth && w > 0 && w < 1e6)
    {
        const float2 ndc = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
        const float3 p = float3(ndc.x * w / p00, ndc.y * w / p11, w);
        const float dist = length(p);
        // quatre rayons répartis dans le disque solaire (strates 2×2 tournées par pixel) : pénombre lisse
        const float3 t1 = normalize(abs(sun.y) < 0.99 ? cross(sun, float3(0, 1, 0)) : cross(sun, float3(1, 0, 0)));
        const float3 t2 = cross(sun, t1);
        const float rot = 6.2831853 * frac(52.9829189 * frac(dot(float2(id), float2(0.06711056, 0.00583715))));
        lit = 0.0;
        for (uint k = 0; k < 4; k++)
        {
            const float u = (float(k & 1) + 0.5) * 0.5, v = (float(k >> 1) + 0.5) * 0.5;
            const float r = sunRadius * sqrt(u), a = 6.2831853 * v + rot;
            RayDesc ray;
            ray.Origin = p * (1.0 - bias) + sun * bias * dist;
            ray.Direction = normalize(sun + t1 * (r * cos(a)) + t2 * (r * sin(a)));
            ray.TMin = 0.0;
            ray.TMax = 1e5;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
            q.Proceed();
            lit += q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 0.25;
        }
        if (flags & 1)
        {
            const float game = GameLit(p);
            if (flags & 8)
                lit = game; // contrôle : l'ombre du jeu telle que recalculée
            else if (flags & 4)
                lit = 1.0 - game * (1.0 - lit); // n'ajouter de l'ombre que là où le jeu éclaire
        }
    }
    shadow.Store(id.y * shadowPitch + id.x * 4, asuint(lit));
}

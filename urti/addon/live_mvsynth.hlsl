// URTI en direct : vecteurs de mouvement fabriqués à partir du mouvement de la caméra mesuré sur notre géométrie
// (live_camfit) et de la profondeur de cette image, quand ceux du jeu restent illisibles (Horizon Forbidden West :
// aucune convention ne se départage). Pour chaque pixel : son point dans l'espace caméra de cette image, ramené
// dans celui de l'image précédente (p = R^T (q - t)), projeté : là où il était. Ciel : la rotation seule. Juste
// pour ce qui est immobile ; un personnage qui bouge est rejeté par les historiques (écart de profondeur).
// Écrit au format du jeu attendu par les passes qui les lisent : deux demi-flottants, convention 0 (position
// précédente = uv + vecteur, unités de l'écran 0..1).

ByteAddressBuffer depth : register(t0);
ByteAddressBuffer sunBuf : register(t1); // loi de profondeur (octets 36 à 47)
ByteAddressBuffer motion : register(t2); // R (par lignes), t, validité (live_camfit)
RWByteAddressBuffer mv : register(u0);

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint depth24;
    uint mvPitch;
    float p00;
    float p11;
    uint pad0;
};

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    float2 m = 0;
    if (asfloat(motion.Load(48)) > 0.5)
    {
        const float3 law = asfloat(sunBuf.Load3(36));
        const float3 r0 = asfloat(motion.Load3(0)), r1 = asfloat(motion.Load3(12)), r2 = asfloat(motion.Load3(24));
        const float3 t = asfloat(motion.Load3(36));
        const uint raw = depth.Load(id.y * depthPitch + id.x * 4);
        const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
        const float w = law.z > 0 ? law.y / (d - law.x) : -1.0;
        const float clearValue = law.y > 0 ? 0.0 : 1.0;
        const float2 uv = (float2(id) + 0.5) / float2(width, height);
        const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
        float3 q = float3(ndc.x / p00, ndc.y / p11, 1.0);
        const bool sky = !(d != clearValue && w > 0 && w < 1e6);
        if (!sky)
            q *= w;
        // R^T (q - t) : les colonnes de R ; le ciel (à l'infini) ne suit que la rotation
        const float3 a = sky ? q : q - t;
        const float3 p = float3(r0.x * a.x + r1.x * a.y + r2.x * a.z, r0.y * a.x + r1.y * a.y + r2.y * a.z, r0.z * a.x + r1.z * a.y + r2.z * a.z);
        if (p.z > 1e-3)
        {
            const float2 ndcPrev = float2(p.x * p00 / p.z, p.y * p11 / p.z);
            const float2 uvPrev = float2(ndcPrev.x * 0.5 + 0.5, 0.5 - ndcPrev.y * 0.5);
            m = uvPrev - uv;
        }
    }
    mv.Store(id.y * mvPitch + id.x * 4, f32tof16(m.x) | (f32tof16(m.y) << 16));
}

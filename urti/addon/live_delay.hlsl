// URTI en direct : file d'attente des calques. Avec DLSS-G, l'image affichée est en retard d'une à deux images du jeu sur le dernier calcul :
// le calque de chaque image calculée (ombre, reflet stabilisé, mouvement) est rangé ici, dans une des trois tranches d'un même tampon, avec le
// numéro de son image du jeu ; live_composite pose sur chaque présentation le calque de l'image du jeu qu'elle montre (live_hudgain le sait),
// au lieu de faire glisser le dernier calque de deux images de mouvement (à-coups dès que le rythme du jeu varie).
// Tampon : en-tête de 16 octets par tranche (numéro de l'image du jeu, décalage sous-pixel x et y, 1 si garnie), puis à partir de l'octet 64
// les tranches ; par pixel de rendu 20 octets : ombre (16 bits flottants), reflet (4 × 16 bits), mouvement (4 × 16 bits).

ByteAddressBuffer mask : register(t0);     // 1 éclairé, 0 ombre (float, lignes de maskPitch octets)
ByteAddressBuffer resolved : register(t1); // reflet stabilisé : couleur × poids, poids (float4)
ByteAddressBuffer motion : register(t2);   // mouvement de la surface et du reflet (float4)
ByteAddressBuffer sunBuf : register(t3);   // décalage sous-pixel de cette image (octets 1184 à 1191)
RWByteAddressBuffer ring : register(u0);

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint maskPitch;
    uint slice;
    uint serial;
    uint flags;   // 1 : reflet disponible ; 2 : mouvement disponible
    uint pad0;
    uint pad1;
};

uint2 Pack(float4 v)
{
    v = all(isfinite(v)) ? v : 0;
    return uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x == 0 && id.y == 0)
        ring.Store4(slice * 16, uint4(serial, sunBuf.Load2(1184), 1u));
    if (id.x >= width || id.y >= height)
        return;
    const uint src = (id.y * width + id.x) * 16;
    const uint dst = 64 + slice * (width * height * 20) + (id.y * width + id.x) * 20;
    const float m = asfloat(mask.Load(id.y * maskPitch + id.x * 4));
    ring.Store(dst, f32tof16(isfinite(m) ? m : 1.0));
    ring.Store2(dst + 4, (flags & 1) ? Pack(asfloat(resolved.Load4(src))) : uint2(0, 0));
    ring.Store2(dst + 12, (flags & 2) ? Pack(asfloat(motion.Load4(src))) : uint2(0, 0));
}

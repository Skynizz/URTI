// URTI en direct : application des ombres et des reflets RT à l'image présentée, à chaque présentation
// (images du jeu et images générées par DLSS / FSR, qui ne passent pas par les effets de ReShade).
// L'image affichée est recopiée dans un tampon, décodée en lumière linéaire (sRGB, HDR10 PQ ou scRGB) ;
// reflets : couleur lue dans l'image à l'endroit du point reflété, mélangée selon le poids (Fresnel ×
// lissé) ; ombres : assombrissement là où le masque (résolution de rendu, filtré) le demande.

ByteAddressBuffer image : register(t0);      // copie de l'image présentée (lignes alignées)
ByteAddressBuffer mask : register(t1);       // 1 éclairé, 0 ombre ajoutée (float, résolution de rendu)
ByteAddressBuffer reflection : register(t2); // float4 par pixel de rendu : u, v, poids, lissé
ByteAddressBuffer prevMask : register(t3);   // masque de l'image réelle précédente (images générées, mélange)
ByteAddressBuffer resolvedRefl : register(t4); // reflets stabilisés (live_reflresolve) : couleur × poids, poids
ByteAddressBuffer preUi : register(t5);      // photo de l'écran avant l'interface du jeu (bit 10 de debugReflect)
ByteAddressBuffer sunBuf : register(t6);     // décalage sous-pixel du jeu mesuré (octets 1184 à 1191)
RWByteAddressBuffer output : register(u0);   // même disposition que image

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;
    uint format;        // 0 RGBA8, 1 BGRA8, 2 RGB10A2 en HDR10 (PQ), 3 RGBA16F (scRGB)
    uint maskWidth;
    uint maskHeight;
    uint maskPitch;
    float strength;     // assombrissement à l'ombre (0 : rien, 1 : noir)
    uint split;         // 1 : moitié gauche sans RT, trait au centre
    uint showMask;      // 1 : masque d'ombres seul
    float reflStrength; // 0 : pas de reflets RT
    uint debugReflect;  // 1 lissé du matériau, 2 poids des reflets, 3 couleur reflétée seule ; bit 8 : mélanger avec prevMask ;
                        // bit 9 : reflets stabilisés disponibles (resolvedRefl) ; bit 10 : photo avant l'interface (preUi)
    float p00;          // projection supposée (décalage sous-pixel en pixels de rendu)
    float p11;
    float tintR;        // rapport soleil / ciel de chaque canal sur celui de la luminance (live_sunlight) : 1 sans mesure
    float tintG;        // (trois nombres : un float3 ici serait déplacé à la frontière de 16 octets suivante)
    float tintB;
};

// décalage sous-pixel du jeu (jitter, DLSS / TAA), mesuré par live_sunvote dans l'espace caméra d'URTI
// (cisaillement x' = x + a w, y' = y + b w) : en pixels de rendu, le contenu d'un pixel est celui du point
// situé à « pixel - jitter » ; écart d'une image à l'autre (précédente - courante) à 628, absolu à 1184
float2 JitterPixels(float2 ab, float p00_, float p11_, float2 size) { return float2(ab.x * p00_, -ab.y * p11_) * 0.5 * size; }


float3 SrgbToLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float3 LinearToSrgb(float3 c) { return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055); }
float3 PqToNits(float3 e)
{
    const float3 p = pow(saturate(e), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}
float3 NitsToPq(float3 l)
{
    const float3 y = pow(saturate(l / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

uint Address(int2 px) { return uint(px.y) * pitch + uint(px.x) * (format == 3 ? 8 : 4); }

// couleurs lues dans la photo d'avant l'interface quand elle existe (les reflets n'y prennent pas les menus)
static bool gPre = false;

// lumière linéaire (HDR10 : nits / 203, blanc de référence à 1)
float3 LoadLinear(int2 px)
{
    px = clamp(px, int2(0, 0), int2(width - 1, height - 1));
    const uint a = Address(px);
    if (format == 3)
    {
        const uint2 raw = gPre ? preUi.Load2(a) : image.Load2(a);
        return float3(f16tof32(raw.x & 0xFFFF), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFF));
    }
    const uint raw = gPre ? preUi.Load(a) : image.Load(a);
    if (format == 2)
        return PqToNits(float3(raw & 1023, (raw >> 10) & 1023, (raw >> 20) & 1023) / 1023.0) / 203.0;
    const float3 c = float3(raw & 255, (raw >> 8) & 255, (raw >> 16) & 255) / 255.0;
    return SrgbToLinear(format == 1 ? c.bgr : c);
}

void StoreLinear(int2 px, float3 c)
{
    const uint a = Address(px);
    if (format == 3)
    {
        const uint keep = image.Load2(a).y & 0xFFFF0000;
        output.Store2(a, uint2(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | keep));
        return;
    }
    const uint keep = image.Load(a);
    if (format == 2)
    {
        const uint3 q = uint3(round(saturate(NitsToPq(max(c, 0.0) * 203.0)) * 1023.0));
        output.Store(a, q.x | (q.y << 10) | (q.z << 20) | (keep & 0xC0000000));
        return;
    }
    float3 e = LinearToSrgb(saturate(c));
    if (format == 1)
        e = e.bgr;
    const uint3 q = uint3(round(e * 255.0));
    output.Store(a, q.x | (q.y << 8) | (q.z << 16) | (keep & 0xFF000000));
}

float MaskAt(int2 t)
{
    t = clamp(t, int2(0, 0), int2(maskWidth - 1, maskHeight - 1));
    const float m = asfloat(mask.Load(t.y * maskPitch + t.x * 4));
    // image générée (à mi-chemin entre deux images réelles) : moyenne des deux derniers masques
    return (debugReflect & 0x100) ? 0.5 * (m + asfloat(prevMask.Load(t.y * maskPitch + t.x * 4))) : m;
}

// couleur bilinéaire de l'image à une position (0..1)
float3 SampleLinear(float2 uv)
{
    const float2 q = uv * float2(width, height) - 0.5;
    const int2 b = int2(floor(q));
    const float2 f = q - b;
    return lerp(lerp(LoadLinear(b), LoadLinear(b + int2(1, 0)), f.x), lerp(LoadLinear(b + int2(0, 1)), LoadLinear(b + int2(1, 1)), f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const int2 px = int2(id);
    // interface du jeu (pixel différent de la photo d'avant l'interface) : laissée telle quelle, sans ombre
    // ni reflet par-dessus
    if (debugReflect & 0x400)
    {
        const uint a = Address(px);
        if (format == 3 ? any(image.Load2(a) != preUi.Load2(a)) : image.Load(a) != preUi.Load(a))
        {
            if (format == 3)
                output.Store2(a, image.Load2(a));
            else
                output.Store(a, image.Load(a));
            return;
        }
        gPre = true;
    }
    if (split != 0 && id.x == width / 2)
    {
        StoreLinear(px, float3(1, 1, 1));
        return;
    }
    float3 c = LoadLinear(px);
    if (split != 0 && id.x < width / 2)
    {
        StoreLinear(px, c);
        return;
    }

    // masque d'ombres : bilinéaire à la résolution de rendu, adouci par une tente (bords moins durs)
    // masque et reflets calculés sur la profondeur décalée du jeu ; l'image présentée ne l'est pas (TAA) :
    // le point vu en (id) est dans le masque en (id) + décalage
    const float2 jitter = JitterPixels(asfloat(sunBuf.Load2(1184)), p00, p11, float2(maskWidth, maskHeight));
    const float2 m = (float2(id) + 0.5) / float2(width, height) * float2(maskWidth, maskHeight) - 0.5 + jitter;
    const int2 b = int2(floor(m));
    const float2 f = m - b;
    float lit = 0, wsum = 0;
    for (int dy = -1; dy <= 2; dy++)
        for (int dx = -1; dx <= 2; dx++)
        {
            const float wx = max(0.0, 1.5 - abs(dx - f.x)), wy = max(0.0, 1.5 - abs(dy - f.y));
            lit += MaskAt(b + int2(dx, dy)) * wx * wy;
            wsum += wx * wy;
        }
    lit /= wsum;

    // reflet : pixel de rendu correspondant
    // (lu seulement si les reflets sont actifs : sinon le tampon lié n'a pas cette taille, et une lecture
    // hors d'un tampon racine fait tomber le GPU)
    float4 rf = float4(0, 0, 0, 0);
    if (reflStrength > 0 || (debugReflect & 0xFF) != 0)
    {
        const int2 rp = clamp(int2((float2(id) + 0.5) / float2(width, height) * float2(maskWidth, maskHeight) + jitter), int2(0, 0), int2(maskWidth - 1, maskHeight - 1));
        rf = asfloat(reflection.Load4((uint(rp.y) * maskWidth + uint(rp.x)) * 16));
    }
    if ((debugReflect & 0xFF) == 1)
    {
        StoreLinear(px, rf.www);
        return;
    }
    if ((debugReflect & 0xFF) == 2)
    {
        StoreLinear(px, rf.zzz);
        return;
    }
    if (showMask != 0)
    {
        StoreLinear(px, lit.xxx);
        return;
    }
    // l'ombre ne retire que la lumière directe qui éclaire la surface : elle s'applique avant le reflet, qui
    // la remplace en partie (une flaque à l'ombre reflète toujours le ciel et les objets éclairés). Le masque
    // donne la part a de la luminance qui vient du soleil (a = x / (1 + x), x = soleil / ciel × N·L) ; par
    // canal, x est multiplié par la teinte mesurée : le soleil est plus chaud que le ciel, l'ombre bleuit
    const float sunPart = min(strength * (1.0 - lit), 0.98);
    const float3 x = sunPart / (1.0 - sunPart) * float3(tintR, tintG, tintB);
    c *= 1.0 - x / (1.0 + x);
    if (reflStrength > 0 && (debugReflect & 0x200))
    {
        // reflet stabilisé, bilinéaire à la résolution de rendu (couleur prémultipliée par son poids)
        const float2 q = (float2(id) + 0.5) / float2(width, height) * float2(maskWidth, maskHeight) - 0.5 + jitter;
        const int2 b0 = int2(floor(q));
        const float2 f0 = q - b0;
        float4 r = 0;
        for (int j = 0; j < 4; j++)
        {
            const int2 t = clamp(b0 + int2(j & 1, j >> 1), int2(0, 0), int2(maskWidth - 1, maskHeight - 1));
            r += asfloat(resolvedRefl.Load4((uint(t.y) * maskWidth + uint(t.x)) * 16)) * (((j & 1) ? f0.x : 1 - f0.x) * ((j >> 1) ? f0.y : 1 - f0.y));
        }
        if ((debugReflect & 0xFF) == 3)
        {
            StoreLinear(px, r.rgb);
            return;
        }
        if ((debugReflect & 0xFF) == 4)
        {
            StoreLinear(px, saturate(r.a * reflStrength).xxx); // diagnostic : part du reflet RT réellement appliquée
            return;
        }
        const float a = saturate(r.a * reflStrength);
        c = c * (1.0 - a) + r.rgb * saturate(reflStrength);
    }
    else if (reflStrength > 0 && rf.z > 0)
    {
        const float3 reflected = SampleLinear(rf.xy);
        if ((debugReflect & 0xFF) == 3)
        {
            StoreLinear(px, reflected * rf.z);
            return;
        }
        c = lerp(c, reflected, saturate(rf.z * reflStrength));
    }
    StoreLinear(px, c);
}

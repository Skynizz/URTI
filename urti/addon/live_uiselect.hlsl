// URTI en direct : image d'avant l'interface du jeu, choisie sur le GPU dans la même image (l'ordre des
// photos change d'une image à l'autre quand le jeu enregistre ses commandes sur plusieurs fils). Photo de
// cette image : identique à plus de 60 % à l'image finale, et plus proche d'elle que de l'image affichée
// précédente. Parmi elles, la moins identique : la plus tôt, celle qui a le moins d'interface. Image
// immobile (joueur qui regarde, menu ouvert) : chaque photo est aussi proche de l'image précédente que de la
// courante, la règle ne tranche plus ; la photo choisie à l'image d'avant est alors gardée. Aucune :
// l'image finale elle-même (l'interface n'est alors ni reconnue ni épargnée ; l'add-on efface ses reflets).

ByteAddressBuffer finalImage : register(t0);
ByteAddressBuffer snap0 : register(t1);
ByteAddressBuffer snap1 : register(t2);
ByteAddressBuffer snap2 : register(t3);
ByteAddressBuffer snap3 : register(t4);
RWByteAddressBuffer preUi : register(u0);   // même disposition que l'image
RWByteAddressBuffer counts : register(u1);  // live_uicompare (0 à 35) ; choix + 1 des images paires / impaires (64, 68)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;
    uint bytesPerPixel;
    uint count;
    uint parity;       // image paire ou impaire : le choix de l'image précédente est dans l'autre case
    uint pad1;
    uint pad2;
};

int Choose()
{
    const float total = max(float(counts.Load(32)), 1.0);
    int best = -1;
    float bestSame = 2.0;
    for (uint k = 0; k < min(count, 4u); k++)
    {
        const float same = float(counts.Load(k * 4)) / total, old = float(counts.Load(16 + k * 4)) / total;
        if (same >= 0.6 && same > old && same < bestSame)
            best = int(k), bestSame = same;
    }
    const uint previous = counts.Load(64 + (1 - parity) * 4);
    if (best < 0 && previous > 0 && previous - 1 < min(count, 4u))
    {
        const uint k = previous - 1;
        const float same = float(counts.Load(k * 4)) / total, old = float(counts.Load(16 + k * 4)) / total;
        // encore assez proche de l'image finale (un menu peut en couvrir une grande part), et pas une copie de
        // l'image affichée précédente (photo prise trop tôt : elle contiendrait l'ancienne image et ses menus)
        if (same >= 0.3 && (same >= old - 0.03 || old < 0.995))
            best = int(k);
    }
    return best;
}

uint2 Pixel(ByteAddressBuffer b, uint a) { return bytesPerPixel == 8 ? b.Load2(a) : uint2(b.Load(a), 0); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint a = id.y * pitch + id.x * bytesPerPixel;
    const int k = Choose();
    if (id.x == 0 && id.y == 0)
        counts.Store(64 + parity * 4, uint(k + 1));
    const uint2 v = k == 0 ? Pixel(snap0, a) : k == 1 ? Pixel(snap1, a) : k == 2 ? Pixel(snap2, a) : k == 3 ? Pixel(snap3, a) : Pixel(finalImage, a);
    if (bytesPerPixel == 8)
        preUi.Store2(a, v);
    else
        preUi.Store(a, v.x);
}

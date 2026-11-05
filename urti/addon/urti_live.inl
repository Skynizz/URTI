// URTI en direct (inclus dans urti_addon.cpp) : ombres du soleil en ray tracing à chaque image.
//
// À chaque image : les dessins sur la profondeur de l'écran sont rejoués dans le flux (comme une
// capture) ; avant les effets de ReShade, sur sa liste de commandes : sommets vers l'espace caméra,
// structure d'accélération, un rayon par pixel vers le soleil, image d'ombres lue par l'effet urti.fx.
// La loi de profondeur et la carte d'ombres du soleil viennent d'un étalonnage (capture complète,
// cartes d'ombres comprises) fait à l'activation et sur F6 ; ensuite la direction du soleil est
// mesurée à chaque image sur le GPU (paires de dessins écran / carte d'ombres), sans relecture.
//
// Touches : F7 active / coupe ; F6 refait l'étalonnage. Fichier urti_photo à côté de l'add-on : image de
// l'écran après les effets (urti_photoN.ppm).

#include "live_convert_cs.h" // g_live_convert
#include "live_shadow_cs.h"  // g_live_shadow
#include "live_sunfit_cs.h"  // g_live_sunfit
#include "live_sunvote_cs.h" // g_live_sunvote
#include "live_composite_cs.h" // g_live_composite
#include "live_reflect_cs.h"   // g_live_reflect
#include "live_depthfit_cs.h"  // g_live_depthfit
#include "live_mvcal_cs.h"     // g_live_mvcal
#include "live_reflresolve_cs.h" // g_live_reflresolve
#include "live_shadowtemporal_cs.h" // g_live_shadowtemporal
#include "live_regionstats_cs.h" // g_live_regionstats
#include "live_uicompare_cs.h" // g_live_uicompare
#include "live_uiselect_cs.h" // g_live_uiselect
#include "live_sunlight_cs.h" // g_live_sunlight
#include "live_counts_cs.h" // g_live_counts
#include "live_surfcache_cs.h" // g_live_surfcache
#include "live_camfit_cs.h" // g_live_camfit
#include "live_mvsynth_cs.h" // g_live_mvsynth
#include "live_planes_cs.h" // g_live_planes

struct LiveState
{
    bool enabled = false, calibrated = false, bound = false;
    bool depthReady = false; // loi de profondeur connue (reflets) ; calibrated : soleil connu (ombres)
    // étalonnage stocké (urti_calib.ini) : propre au jeu, relu au lancement -> ombres dès la première image
    bool fromStore = false, verified = false, storeLoaded = false;
    uint32_t sunMapWidth = 0, sunMapHeight = 0, sunMapFormat = 0;
    float sun[3] = { 0, 1, 0 }, A = 0, B = 0.1f, p00 = 1, p11 = 1;
    uint64_t nextCalibration = 0;
    uint32_t calibrationInterval = 180;

    ComPtr<ID3D12RootSignature> convertRs, shadowRs;
    ComPtr<ID3D12PipelineState> convertPso, shadowPso;
    ComPtr<ID3D12Resource> view, blas, blasScratch, tlas, tlasScratch, instances, depthBuf, shadowBuf, mask, counterReadback[3];
    ComPtr<ID3D12Resource> prevShadowBuf; // masque de l'image réelle précédente (images générées : mélange)
    ComPtr<ID3D12Resource> shadowRawBuf;  // masque brut de cette image (avant débruitage)
    ComPtr<ID3D12RootSignature> denoiseRs;
    ComPtr<ID3D12PipelineState> denoisePso;
    // budget de géométrie : chaque objet capté est mesuré (distance, taille) ; seuls les plus importants
    // entrent dans la scène RT, jusqu'à vertexBudget sommets ; les autres sont re-mesurés par roulement
    // (probeBudget sommets par image, hors de la structure)
    // photo d'avant l'interface : photos de l'écran (urti_addon.cpp), comparaison, choix
    ComPtr<ID3D12Resource> snaps[kLiveSnaps], uiCounts, uiReadback[3], preUiBuf;
    ComPtr<ID3D12RootSignature> uiRs, uiSelRs;
    ComPtr<ID3D12PipelineState> uiPso, uiSelPso;
    uint64_t snapBytes = 0, uiFrame = 0;
    uint32_t uiCountAt[3] = {};
    int uiChoice = -1; // rang de la photo d'avant l'interface (-1 : aucune sûre), lu trois images plus tard
    uint32_t uiParity[3] = {};
    float uiReflGate = 1.0f; // reflets RT effacés en douceur quand aucune photo sûre (sinon l'interface s'y reflète)
    uint32_t uiBadReadbacks = 0; // photos périmées de suite (lues trois images plus tard)
    float uiZone[4] = {}; // urti.ini ui_zone=x0,y0,x1,y1 (fractions de l'écran) : zone d'interface jamais retouchée
    ComPtr<ID3D12Resource> uiMaskBuf; // pixels d'interface retenus de la dernière photo sûre (live_composite)
    uint64_t uiMaskBytes = 0;
    bool uiPhoto = true; // urti.ini ui_photo=0 : sans la photo d'avant l'interface (contrôle)
    ComPtr<ID3D12RootSignature> statsRs;
    ComPtr<ID3D12PipelineState> statsPso;
    ComPtr<ID3D12Resource> statsBuf, statsUpload[3], statsReadback[3];
    uint64_t statsBytes = 0;
    std::vector<uint32_t> statsIds[3], statsCaps[3]; // identités mesurées et leur place réservée (sommets)
    std::unordered_map<uint32_t, uint32_t> idSig;        // ExecuteIndirect : identité -> signature de commande
    std::unordered_map<uint32_t, float> sigVertices;     // signature -> plus grand nombre de sommets écrits (décroît)
    struct ObjectStat
    {
        float importance;  // taille / distance (taille apparente)
        uint64_t frame;    // image de la dernière mesure
        uint32_t vertices; // sommets réellement écrits (mesurés)
    };
    std::unordered_map<uint32_t, ObjectStat> objects;
    std::unordered_map<uint32_t, uint32_t> seenNow;
    std::unordered_set<uint32_t> admitted;
    uint32_t churnIn = 0, churnOut = 0; // objets entrés dans la scène RT / sortis, à cette image
    uint32_t burst = 0, burstIndex = 0, burstCount = 0; // rafale d'images avant / après URTI en cours (images restantes)
    std::string burstFolder;
    uint64_t statChurn = 0; // objets entrés ou sortis de la scène RT (bilan par 300 images)
    bool composedNow = false;           // la composition d'URTI a eu lieu à cette présentation
    std::atomic<bool> showGameOnly{ false }; // F4 : image du jeu seule (comparaison avant / après en plein écran)
    int menuOn = -1; // case « URTI activé » du menu (UrtiOn) telle que lue à la dernière présentation ; -1 : pas encore lue
    uint64_t vertexBudget = 6000000, probeBudget = 1000000, slotLimit = 0;
    uint64_t statDraws = 0, statOverflow = 0, statOutOfBudget = 0, statProbes = 0, statBudgetFrames = 0, statReserved = 0;
    // accumulation dans le temps : vecteurs de mouvement du jeu (G-buffer), profondeur précédente
    ComPtr<ID3D12Resource> mvBuf, depthPrevBuf, mvErrors, mvErrorsReadback[3];
    ComPtr<ID3D12RootSignature> mvcalRs;
    ComPtr<ID3D12PipelineState> mvcalPso;
    uint64_t mvBytes = 0, mvFrame = ~0ull;
    uint32_t mvPitch = 0, mvWidth = 0, mvHeight = 0, mvConvention = 255, mvMovingFrames = 0;
    // cible essayée parmi celles du G-buffer en deux flottants 16 bits (Horizon Forbidden West en a deux, la
    // première presque vide : toutes les conventions y restaient à égalité) ; mesures sans vainqueur sur celle-ci
    uint32_t mvCandidate = 0, mvUndecided = 0;
    // mouvement de la caméra mesuré sur notre géométrie (live_camfit) et vecteurs de mouvement fabriqués avec lui
    // (live_mvsynth) quand ceux du jeu restent illisibles ; urti.ini motion_source= 0 : choix automatique, 1 : ceux
    // du jeu seulement, 2 : les nôtres
    ComPtr<ID3D12RootSignature> camfitRs, mvsynthRs;
    ComPtr<ID3D12PipelineState> camfitPso, mvsynthPso;
    ComPtr<ID3D12Resource> pairsBuf, motionBuf, motionReadback[3];
    bool mvSynthetic = false;
    // couche des surfaces transparentes planes (eau, vitres : reflets) ; état de son tampon de sommets
    ComPtr<ID3D12RootSignature> planesRs;
    ComPtr<ID3D12PipelineState> planesPso;
    ComPtr<ID3D12Resource> viewT, blasT, blasTScratch, transpUpload[3], transpSrcUpload[3], planeStats, planeReadback[3];
    bool viewTRead = false, transpBuilt = false, waterOn = true;
    uint64_t planeFrames = 0, planeKept = 0, planeTris = 0, planeSeen = 0;
    uint32_t mvGiveUps = 0, motionSource = 0, motMovingNoGame = 0; // images où la caméra bougeait sans convention du jeu trouvée
    double motRot = 0, motTrans = 0, motKept = 0, motRms = 0;
    uint32_t motFrames = 0, motValid = 0;
    double mvErr[16] = {}, mvCount[16] = {};
    float temporalAlpha = 0.3f;
    float reflAlphaMin = 0.1f, reflAlphaMax = 0.5f; // reflets : part de l'image courante, à l'arrêt et en mouvement rapide
    float reflGamma = 1.0f, reflDepthTol = 0.1f;    // reflets : largeur du bornage (écarts-types), tolérance de profondeur
    uint32_t debugView = 0;    // vue de diagnostic (menu UrtiDebug ou urti.ini debug_view=)
    int iniDebugView = -1;
    // reflets stabilisés : historique à la résolution de rendu (deux tampons alternés)
    ComPtr<ID3D12RootSignature> resolveRs;
    ComPtr<ID3D12PipelineState> resolvePso;
    ComPtr<ID3D12Resource> reflHist[2];
    uint64_t reflHistBytes = 0;
    int reflHistLast = -1; // tampon écrit à la dernière image réelle
    uint32_t fgMode = 0;                  // images générées : 0 même masque, 1 sans RT, 2 mélange des deux dernières
    // stabilité du soleil : écart d'une image mesurée à la suivante
    float lastSun[3] = {};
    double sunJitterSum = 0, sunJitterMax = 0;
    uint32_t sunJitterCount = 0;
    uint64_t capacity = 0; // sommets de la structure
    uint32_t width = 0, height = 0, depthPitch = 0, shadowPitch = 0; // width, height : zone rendue (résolution de rendu)
    uint32_t allocWidth = 0, allocHeight = 0; // taille de la profondeur (images allouées à cette taille)
    float viewMax[2] = {};                    // zone de dessin du jeu mesurée (dernière fenêtre)
    uint32_t viewLogCount = 0, viewCandidate = 0, viewStreak = 0;
    bool renderAreaOn = true; // urti.ini render_area=0 : toute la profondeur (essai)
    bool aoRough = false;     // urti.ini ao_rough=1 : occultation aussi sur les surfaces rugueuses (herbe), pente d'ensemble
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    resource_view maskSrv = {};
    uint64_t frame = 0, lastFilled = 0;
    // structure d'accélération : mise à jour (refit, plusieurs fois moins chère) tant que la liste des
    // dessins est la même qu'à la dernière construction, reconstruite au plus tous les blasRefit images
    uint64_t blasLayout = 0;
    // blasRefit : mises à jour de suite au plus ; 0 par défaut : dans RDR2 une seule mise à jour rendait les rayons
    // de reflet quatorze fois plus lents (lots de végétation triés par le GPU : les triangles changent de place)
    uint32_t blasAge = ~0u, blasRefit = 0, statRebuilds = 0, statUpdates = 0;
    uint64_t statVertices = 0;
    std::vector<ComPtr<ID3D12Resource>> retired[3]; // libérées trois images plus tard
    uint32_t photoIndex = 0;
    // mémoire géométrique : sommets de l'image précédente (espace caméra de cette image-là) et, par
    // identité d'objet, où commençait sa zone ; un point touché retrouve ainsi sa position précédente,
    // déformations comprises (vent, animation : les sommets sont captés après le vertex shader)
    ComPtr<ID3D12Resource> viewPrev, linkUpload[3], anchor[2];
    std::unordered_map<uint32_t, uint32_t> prevStart, pendingStart;
    bool prevValid = false;
    int anchorOut = -1; // ancrage écrit à cette image (anchor[anchorOut]), l'autre est celui de la précédente
    uint64_t anchorFrame = ~0ull;
    bool anchorPrevOk = false; // l'autre tampon a bien été écrit à l'image précédente
    uint64_t anchorBytes = 0;
    // ancrage coupé par défaut : mesuré sur le bac à sable, il ne réduit pas le clignotement (le passage
    // d'un filtre à l'autre selon les pixels en ajoute) ; anchor_alpha=0.1 dans urti.ini pour l'essayer
    float anchorAlpha = 0.0f, anchorGamma = 2.5f;
    bool alphaScreen = true; // trous des plaques découpées reconnus à l'écran (urti.ini alpha_screen=0 : coupé)
    // ombres débruitées par SIGMA de NVIDIA (urti.ini nrd=1) si l'add-on a NRD ; coupé par défaut tant qu'il ne bat pas
    // le débruitage d'URTI sur le bac à sable (il lui manque le décalage sous-pixel du jeu, inconnu pour l'instant)
    bool nrdShadows = false;
    uint32_t statNrd = 0;
    float nrdDisocclusion = 0.01f, nrdFrames = 5.0f;
    bool jitterLog = false;
    float fovStretch = 0; // t² mesuré par live_sunvote (étirement de la projection supposée)
    bool fovAuto = true;  // projection corrigée par le champ de vision mesuré (urti.ini fov_auto=0 : 50° supposés)
    bool fovReset = false; // sommes de la mesure à remettre à zéro (projection changée)
    uint32_t fovHold = 0;  // images à ignorer après une correction (mesures lues avec trois images de retard)
    // part du soleil dans l'image du jeu (live_sunlight) : rapport soleil / ciel par canal (rouge, vert, bleu,
    // luminance), lissé ; les ombres RT ne retirent que cette part (urti.ini sun_measure=0 : pente fixe)
    bool sunMeasure = true;
    uint32_t gameDilate = 1; // urti.ini game_dilate= : voisinage lu dans la carte du jeu (rayon en texels)
    bool gameMin = true;
    float aoAmount = 0.8f, aoRadius = 1.5f; // occultation ambiante RT (urti.ini ao=, ao_radius= ; ao=0 : coupée)
    // ombres du soleil RT : coupées par défaut (urti.ini shadows=1) : dans RDR2, ses ombres déjà excellentes n'en
    // laissent presque aucune à ajouter (rafale au soleil : 0,0 % de l'image changé pour 3,2 ms par image)
    bool sunShadows = false;
    bool aoQuarter = true; // urti.ini ao_quarter=0 : occultation calculée sur chaque pixel à chaque image
    // rayons d'occultation par pixel calculé (au quart), de 2 à 4, réglés toutes les 30 images sur le temps mesuré
    // de la passe des ombres (urti.ini ao_ms=, 0 : toujours 4)
    float aoTarget = 2.5f;
    uint32_t aoRays = 4;
    double shadowMs = 0;
    // budget de géométrie réglé sur le coût mesuré de la reconstruction (urti.ini build_ms=, 0 : budget fixe) :
    // au-dessus de la cible, il baisse (les objets de plus petite taille apparente sortent d'abord), en dessous
    // il remonte jusqu'à vertex_budget
    float buildTarget = 2.5f;
    uint64_t budgetMax = 6000000;     // urti.ini game_min=0 : ombre du jeu élargie (tout ou rien) et produit des visibilités
    float sunSky[4] = { 2.2f, 2.2f, 2.2f, 2.2f };
    uint32_t sunSkyFrames = 0, sunSkyLogged = 0;
    bool sunWeakLogged = false;
    uint64_t sunSkyPairs = 0, atlasFrame = ~0ull;
    uint64_t mapCheck[6] = {}; // contrôle de la carte du jeu : points éclairés, à l'ombre, au bord, hors des cascades ; luminances (× 1024)
    uint64_t depthHist[16] = {}, gapHist[16] = {}; // profondeur des points dans la cascade, écart carte - point
    ComPtr<ID3D12RootSignature> sunlightRs;
    ComPtr<ID3D12PipelineState> sunlightPso;
    uint32_t jitterMode = 2; // décalage sous-pixel du jeu compensé : 0 non, 1 écart d'une image à l'autre, 2 aussi l'absolu // SIGMA : seuil de disocclusion, images stabilisees
    uint64_t boundFrame = 0;

    // suivi du soleil à chaque image
    bool forceCalibration = false, sunDirty = false;
    int iniCombine = -1; // urti.ini combine= : 0 ou 1 impose le mode ; -1 : menu de ReShade (UrtiMode)
    std::atomic<bool> overlayOpen{ false }; // menu de ReShade ouvert : pas de passe RT (le menu doit rester visible)
    uint32_t shadowRays = 4;                // urti.ini shadow_rays= (1 à 4)
    // minutage GPU par étape (horodatages), moyenné sur 300 images
    ComPtr<ID3D12QueryHeap> timeHeap;
    ComPtr<ID3D12Resource> timeReadback[3];
    double timeSum[8] = {};
    uint32_t timeFrames = 0;
    uint32_t lastPresentDraws = 1; // dessins du jeu exécutés avant cette présentation (0 : rien de neuf à calculer)
    // présentations récentes où le jeu avait dessiné dans l'image présentée (bit 0 : celle-ci) ; image présentée
    // fabriquée par la génération d'images (pas par le jeu)
    uint32_t backBufferDrawn = 0, presentNoDraw = 0; // et présentations récentes sans dessin du jeu exécuté
    bool imageGenerated = false;
    // reflets : matériau du G-buffer copié à chaque image, rayons réfléchis
    ComPtr<ID3D12RootSignature> reflectRs, depthfitRs;
    ComPtr<ID3D12PipelineState> reflectPso, depthfitPso;
    ComPtr<ID3D12Resource> materialBuf, reflectBuf;
    uint64_t materialBytes = 0, materialFrame = ~0ull, reflectBytes = 0;
    uint32_t materialPitch = 0, materialWidth = 0, materialHeight = 0, materialFormat = 0;
    float reflStrength = 1.0f, smoothMin = 0.5f, f0 = 0.04f, reflEffective = 1.0f;
    uint32_t reflChannel = 1, reflInvert = 0, debugReflect = 0, materialSlot = 2, reflEverywhere = 0;
    // cache de surfaces (live_surfcache) : couleur vue à l'écran de chaque triangle de la scène RT, pour les
    // reflets qui touchent un objet caché ou hors de l'écran (urti.ini surf_cache=0 : coupé)
    ComPtr<ID3D12RootSignature> surfRs;
    ComPtr<ID3D12PipelineState> surfPso;
    ComPtr<ID3D12Resource> surfCache;
    uint64_t surfTriangles = 0, surfFilled = 0; // triangles tenus ; images où le cache a été rempli
    bool surfCacheOn = true;
    // scène RT de cette image, pour le remplissage du cache à la composition : table des zones (tampon
    // d'envoi tournant sur trois images), nombre de zones, image où elle a servi
    ID3D12Resource* sceneTable = nullptr;
    uint32_t sceneCount = 0;
    ComPtr<ID3D12Resource> sceneGpu; // la table copiée en mémoire vidéo à chaque image (lue en SRV jusqu'à la suivante)
    bool sceneGpuReadable = false;
    // rebond de lumière (éclairage indirect, urti.ini gi=, 0 : coupé) : couleur des surfaces touchées par les rayons
    // d'occultation, tirée du cache de surfaces ; trois plans R, G, B à la résolution de rendu : brut (live_shadow),
    // débruité (live_shadowtemporal), précédent (historique). Reçu selon la couleur propre des surfaces (albedo du
    // G-buffer, copiée à chaque image)
    float giStrength = 1.0f;
    bool personMask = true;     // urti.ini person_mask= : alpha de la normale du G-buffer = personnages (RDR2) ; 0 : pas de repère
    ComPtr<ID3D12Resource> giRaw, giBuf, giPrev;
    bool giValid = false;       // giBuf écrit à la dernière image (sinon pas d'historique, pas de pose)
    uint64_t giStamp = ~0ull;
    ComPtr<ID3D12Resource> albedoBuf;
    uint64_t albedoBytes = 0, albedoFrame = ~0ull;
    uint32_t albedoPitch = 0, albedoWidth = 0, albedoHeight = 0, albedoFormat = 0;
    // normale du G-buffer (cible 1) ; son alpha repère les personnages (RDR2), qui ne reçoivent pas le rebond : ils
    // bougent, leur historique est rejeté à chaque image et le rebond, plus bruité que l'occultation, y tremblait
    ComPtr<ID3D12Resource> normalBuf;
    uint64_t normalBytes = 0, normalFrame = ~0ull;
    uint32_t normalPitch = 0, normalWidth = 0, normalHeight = 0, normalFormat = 0;
    uint64_t sceneStamp = ~0ull;
    uint64_t maskFrame = ~0ull;    // image où le masque d'ombres a été calculé (sinon il ne suit plus la caméra)
    int lastMode = -1;
    float lastStrength = -1;
    bool lastSplit = false, lastShowMask = false, lastTechnique = true, iniMaskSet = false;
    std::atomic<bool> toggleSplit{ false }; // F5 : écran partagé (gauche jeu seul, droite avec ombres RT)
    float sunSign = 1; // sens du soleil par rapport à la profondeur des cartes (convention du jeu)
    uint32_t missedCalibrations = 0;
    ComPtr<ID3D12RootSignature> fitRs, voteRs;
    ComPtr<ID3D12PipelineState> fitPso, votePso;
    ComPtr<ID3D12Resource> pairsUpload[3], pairDirs, sunBuf, sunUpload, sunReadback[3];
    ComPtr<ID3D12Resource> regionUpload[3]; // table des zones de l'écran pour le GPU (par image)
    // places fixes (g_livePersist) : table de toutes les places tenues, comptes de sommets gardés d'une image à
    // l'autre (8 octets par place), paires (compteur de l'image, case de compte) des dessins de l'image
    ComPtr<ID3D12Resource> placeUpload[3], srcUpload[3], fitUpload[3];
    std::unordered_map<uint32_t, uint32_t> idMesh; // identité d'un exemplaire -> son maillage (budget par maillage)
    ComPtr<ID3D12RootSignature> countsRs;
    ComPtr<ID3D12PipelineState> countsPso;
    uint32_t blasConverted = 0; // sommets de la dernière construction complète (mise à jour : même nombre)
    // places redistribuées à la fin de l'image précédente, ou beaucoup de nouvelles dans celle-ci : les triangles
    // ne sont plus là où la structure les attend, une mise à jour la rendrait très lente (v58 : 4,5 i/s) ;
    // reconstruction complète
    bool placesMoved = false, placesResetPending = false;
    // mise à jour ou reconstruction, au coût mesuré (chronomètres du GPU lus trois images plus tard) : une mise à
    // jour coûte moins cher que la reconstruction, mais ralentit les rayons ; dès que leur ralentissement
    // cumulé depuis la dernière reconstruction dépasse ce qu'elle épargne, la structure est reconstruite
    uint32_t buildCycle = 0, slotCycle[3] = {};
    bool slotRefit[3] = {}, slotBuilt[3] = {};
    double rebuildMs = 2.0, refitMs = 0.5, traceBase = -1.0, refitExtra = 0.0;
    uint64_t statCycles = 0;
    uint64_t statPlaces = 0, statPlaceHeld = 0, statPlaceHigh = 0, statPlaceNew = 0, statPlaceFrames = 0, statPlaceResets = 0;
    ComPtr<ID3D12Resource> atlasBuf; // copie de la carte d'ombres du jeu, pour recalculer son ombre
    // application à l'image présentée (toutes les présentations, images générées comprises)
    ComPtr<ID3D12RootSignature> compositeRs;
    ComPtr<ID3D12PipelineState> compositePso;
    ComPtr<ID3D12Resource> imageIn, imageOut;
    uint64_t imageBytes = 0;
    // bilan par 300 présentations : sans dessin du jeu, effets de ReShade, ombres appliquées
    uint32_t statPresents = 0, statNoDraw = 0, statEffects = 0, statComposited = 0;
    // rechargement à chaud : réglages (urti.ini) et shaders (urti_shaders\*.cso), sans relancer le jeu
    FILETIME iniTime = {}, shaderTime[5] = {};
    std::vector<ComPtr<ID3D12PipelineState>> retiredPso[3];
    uint64_t atlasBytes = 0;
    uint32_t atlasPitch = 0;
    bool atlasD24 = false, atlas16 = false; // carte d'ombres du jeu en D24, en 16 bits (RDR2 : R16, deux octets par texel)
    uint64_t framesSinceSun = 0, sunFrames = 0, sunAccepted = 0, pairSum = 0;
    uint32_t rotationValid = 0;
    double rotationSum = 0;
} live;
constexpr uint32_t kLiveMaxPairs = 256;
// soleil, bilan, cascades (48 + 64 k), soleil en attente (560), rotation de la caméra (576), cascades de
// l'image précédente (640)
constexpr uint32_t kLiveMaxMotionPairs = 32768; // paires de sommets pour le mouvement de la caméra (live_camfit)
constexpr uint32_t kLiveSunBytes = 2048; // + suivi du décalage sous-pixel du jeu (1168 à 1199), étirement de la projection (1200 à 1223),
                                                             // part du soleil dans l'image (1232 : paires, 1248 : histogrammes, 1888 à 2047 : contrôle, live_sunlight)

// ------------------------------------------------------------------------------------------------
// étalonnage stocké : loi de profondeur, convention de la carte d'ombres, description de cette carte
// ------------------------------------------------------------------------------------------------

void LiveSaveCalibration()
{
    // loi de profondeur invalide : rien n'est écrit (relue au lancement, une loi NaN rendait chaque point de l'écran
    // invalide : plus d'occultation ni de reflet dans Horizon Forbidden West, jusqu'à la mesure de l'image)
    if (!std::isfinite(live.A) || !std::isfinite(live.B) || live.B == 0)
        return;
    if (FILE* f = std::fopen((g_dir + "\\urti_calib.ini").c_str(), "w"))
    {
        std::fprintf(f, "; étalonnage URTI de ce jeu (relu au lancement ; F6 le refait)\n");
        std::fprintf(f, "depth_a=%.9g\ndepth_b=%.9g\nsun_sign=%d\nsun_map=%ux%u:%u\nmotion=%u\nmotion_target=%u\nmotion_source=%s\n", live.A, live.B,
                     live.sunSign > 0 ? 1 : -1, live.sunMapWidth, live.sunMapHeight, live.sunMapFormat, live.mvSynthetic ? 255u : live.mvConvention,
                     live.mvCandidate, live.mvSynthetic ? "camera" : "jeu");
        std::fclose(f);
    }
}

// projection supposée (champ de vision arbitraire, comme Calibrate) pour le rapport largeur / hauteur
void LiveSetProjection(const D3D12_RESOURCE_DESC& depthDesc)
{
    const float fov = 50.0f * 3.14159265f / 180.0f;
    const float aspect = depthDesc.Height ? float(depthDesc.Width) / float(depthDesc.Height) : 16.0f / 9.0f;
    live.p11 = 1.0f / std::tan(fov * 0.5f), live.p00 = live.p11 / aspect;
}

// à la première présentation : étalonnage relu ; la carte d'ombres sera reconnue à sa description
void LiveLoadCalibration()
{
    if (live.storeLoaded)
        return;
    live.storeLoaded = true;
    FILE* f = std::fopen((g_dir + "\\urti_calib.ini").c_str(), "r");
    if (!f)
        return;
    double a = 0, b = 0;
    int sign = 0;
    unsigned w = 0, h = 0, fmt = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f))
    {
        std::sscanf(line, "depth_a=%lf", &a), std::sscanf(line, "depth_b=%lf", &b), std::sscanf(line, "sun_sign=%d", &sign);
        std::sscanf(line, "sun_map=%ux%u:%u", &w, &h, &fmt);
        unsigned motionConvention = 255;
        if (std::sscanf(line, "motion=%u", &motionConvention) == 1 && motionConvention < 16)
            live.mvConvention = motionConvention;
        std::sscanf(line, "motion_target=%u", &live.mvCandidate);
        if (std::strncmp(line, "motion_source=camera", 20) == 0 && live.motionSource != 1)
            live.mvSynthetic = true, live.mvConvention = 0;
    }
    std::fclose(f);
    if (b == 0 || !std::isfinite(a) || !std::isfinite(b) || !sign || !w)
        return;
    live.p00 = 0; // fixée à la première profondeur (rapport largeur / hauteur)
    live.A = float(a), live.B = float(b), g.depthReversed = b > 0, live.sunSign = float(sign);
    live.sunMapWidth = w, live.sunMapHeight = h, live.sunMapFormat = fmt;
    live.depthReady = live.calibrated = live.fromStore = true;
    live.framesSinceSun = 1000; // pas d'ombres avant la première mesure du soleil (quelques images)
    g_liveSunMapDesc[0] = w, g_liveSunMapDesc[1] = h, g_liveSunMapDesc[2] = fmt;
    Log("URTI direct : étalonnage stocké relu (profondeur = %.6f + %.6f / w, carte d'ombres %ux%u format %u) : ombres dès la première image",
        a, b, w, h, fmt);
}

// ------------------------------------------------------------------------------------------------
// étalonnage : direction du soleil (cartes d'ombres) et loi de profondeur, sur le processeur
// ------------------------------------------------------------------------------------------------

// moindres carrés S ≈ X M (X : n × 4, S : n × 3) par les équations normales ; faux si mal conditionné
bool SolveAffine(const std::vector<double>& X, const std::vector<double>& S, size_t n, double M[4][3])
{
    double N[4][4] = {}, R[4][3] = {};
    for (size_t i = 0; i < n; i++)
        for (int a = 0; a < 4; a++)
        {
            for (int b = 0; b < 4; b++)
                N[a][b] += X[i * 4 + a] * X[i * 4 + b];
            for (int c = 0; c < 3; c++)
                R[a][c] += X[i * 4 + a] * S[i * 3 + c];
        }
    for (int col = 0; col < 4; col++) // Gauss avec pivot partiel
    {
        int p = col;
        for (int r = col + 1; r < 4; r++)
            if (std::fabs(N[r][col]) > std::fabs(N[p][col]))
                p = r;
        if (std::fabs(N[p][col]) < 1e-12)
            return false;
        for (int k = 0; k < 4; k++)
            std::swap(N[col][k], N[p][k]);
        for (int k = 0; k < 3; k++)
            std::swap(R[col][k], R[p][k]);
        for (int r = 0; r < 4; r++)
        {
            if (r == col)
                continue;
            const double f = N[r][col] / N[col][col];
            for (int k = 0; k < 4; k++)
                N[r][k] -= f * N[col][k];
            for (int k = 0; k < 3; k++)
                R[r][k] -= f * R[col][k];
        }
    }
    for (int a = 0; a < 4; a++)
        for (int c = 0; c < 3; c++)
            M[a][c] = R[a][c] / N[a][a];
    return true;
}

// À partir d'une capture lue (sommets de tous les flux, bornes des dessins) : soleil et profondeur.
void Calibrate(const uint8_t* so, const uint32_t* counters, const uint64_t* marks)
{
    const int mainSlot = g.captureSlot;
    if (mainSlot < 0)
        return;
    const float fov = 50.0f * 3.14159265f / 180.0f; // valeur arbitraire : les ombres n'en dépendent pas
    const float aspect = g.depthDesc.Height ? float(g.depthDesc.Width) / float(g.depthDesc.Height) : 16.0f / 9.0f;
    const float p11 = 1.0f / std::tan(fov * 0.5f), p00 = p11 / aspect;
    auto vertex = [&](int slot, uint64_t byteOffset) { return reinterpret_cast<const float*>(so + slot * kSlotBytes + byteOffset); };

    // loi de profondeur : z de découpage = A w + B, ajustée sur les sommets de l'écran
    double sw = 0, sz = 0, sww = 0, swz = 0, n = 0;
    const uint64_t mainBytes = std::min<uint64_t>(counters[mainSlot * 2], kSlotBytes);
    const uint64_t sampleStep = 16 * std::max<uint64_t>(1, mainBytes / 16 / 20000);
    for (uint64_t o = 0; o + 16 <= mainBytes; o += sampleStep)
    {
        const float* c = vertex(mainSlot, o);
        if (c[3] > 1e-3f)
            sw += c[3], sz += c[2], sww += double(c[3]) * c[3], swz += double(c[3]) * c[2], n += 1;
    }
    if (n < 30)
    {
        Log("URTI direct : étalonnage impossible (%u sommets sur la profondeur de l'écran)", unsigned(n));
        return;
    }
    const double A = (n * swz - sw * sz) / (n * sww - sw * sw), B = (sz - A * sw) / n;
    if (!std::isfinite(A) || !std::isfinite(B) || B == 0)
    {
        Log("URTI direct : étalonnage impossible (loi de profondeur dégénérée)");
        return;
    }
    if (!live.depthReady)
        Log("URTI direct : loi de profondeur = %.6f + %.6f / w (reflets possibles même sans soleil)", A, B);
    live.A = float(A), live.B = float(B), live.p00 = p00, live.p11 = p11;
    g.depthReversed = B > 0;
    live.depthReady = true;

    // soleil : pour chaque carte d'ombres, une direction par paire de dessins, puis vote
    struct Dir { double d[3]; double w; };
    double bestSun[3] = { 0, 1, 0 }, bestScore = 0;
    uint32_t bestSlot = 0;
    for (uint32_t s = 0; s < kSlots; s++)
    {
        if (int(s) == mainSlot || !g.slotDepth[s] || g.screenLike[g.slotDepth[s]] == 1)
            continue;
        std::vector<Dir> dirs;
        std::unordered_map<uint64_t, std::vector<uint32_t>> theirs;
        auto key = [](const DrawRecord& r, uint64_t bytes) {
            uint64_t k = r.kind * 1000003ull + bytes;
            for (uint32_t p : r.params)
                k = k * 1315423911ull + p;
            return k;
        };
        for (uint32_t d = 0; d < g.captureDrawCount; d++)
            if (g.draws[d].slot == s && marks[d * 2 + 1] > marks[d * 2])
                theirs[key(g.draws[d], marks[d * 2 + 1] - marks[d * 2])].push_back(d);
        std::vector<double> X, S;
        std::unordered_map<uint64_t, uint32_t> rank; // rang du dessin parmi ceux de même signature
        for (uint32_t d = 0; d < g.captureDrawCount && dirs.size() < 4000; d++)
        {
            if (g.draws[d].slot != uint32_t(mainSlot) || marks[d * 2 + 1] <= marks[d * 2])
                continue;
            const uint64_t bytes = marks[d * 2 + 1] - marks[d * 2];
            const uint64_t k0 = key(g.draws[d], bytes);
            const uint32_t myRank = rank[k0]++;
            auto it = theirs.find(k0);
            if (it == theirs.end())
                continue;
            const uint64_t verts = bytes / 16, step = std::max<uint64_t>(1, verts / 256);
            for (size_t m = 0; m < it->second.size() && m < 64; m++)
            {
                const uint32_t e = it->second[m];
                X.clear(), S.clear();
                for (uint64_t v = 0; v < verts; v += step)
                {
                    const float* c = vertex(mainSlot, marks[d * 2] + v * 16);
                    const float* l = vertex(int(s), marks[e * 2] + v * 16);
                    if (c[3] <= 1e-3f || std::fabs(l[3]) < 1e-6f)
                        continue;
                    X.insert(X.end(), { c[0] / p00, c[1] / p11, double(c[3]), 1.0 });
                    S.insert(S.end(), { double(l[0]) / l[3], double(l[1]) / l[3], double(l[2]) / l[3] });
                }
                const size_t k = X.size() / 4;
                double M[4][3];
                if (k < 24 || !SolveAffine(X, S, k, M))
                    continue;
                // ajustement fidèle ? (sinon autre objet, ou animé)
                std::vector<double> res(k);
                double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
                for (size_t i = 0; i < k; i++)
                {
                    double r2 = 0;
                    for (int c = 0; c < 3; c++)
                    {
                        double pred = M[3][c];
                        for (int a = 0; a < 3; a++)
                            pred += X[i * 4 + a] * M[a][c];
                        r2 += (pred - S[i * 3 + c]) * (pred - S[i * 3 + c]);
                        lo[c] = std::min(lo[c], S[i * 3 + c]), hi[c] = std::max(hi[c], S[i * 3 + c]);
                    }
                    res[i] = std::sqrt(r2);
                }
                std::nth_element(res.begin(), res.begin() + k / 2, res.end());
                const double scale = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] }) + 1e-12;
                if (res[k / 2] > 1e-3 * scale)
                    continue;
                // d tel que A d = (0, 0, 1), A = M[0..2]ᵀ : la direction de la profondeur de la carte
                const double a[3][3] = { { M[0][0], M[1][0], M[2][0] }, { M[0][1], M[1][1], M[2][1] }, { M[0][2], M[1][2], M[2][2] } };
                const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                                   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
                if (std::fabs(det) < 1e-18)
                    continue;
                // troisième colonne de l'inverse : cofacteurs de la troisième ligne
                const double dx = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
                const double dy = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
                const double dz = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                // même rang dans les deux passes : très probablement le même objet (maillages répétés)
                dirs.push_back({ { -dx / len, -dy / len, -dz / len }, double(k) * (m == myRank ? 8.0 : 1.0) });
            }
        }
        // direction (au signe près) sur laquelle s'accordent le plus de sommets, à 2°
        const double cosTol = std::cos(2.0 * 3.14159265 / 180.0);
        double slotBest = 0;
        size_t bestIndex = 0;
        for (size_t i = 0; i < dirs.size(); i++)
        {
            double vote = 0;
            for (const Dir& o : dirs)
                if (std::fabs(dirs[i].d[0] * o.d[0] + dirs[i].d[1] * o.d[1] + dirs[i].d[2] * o.d[2]) > cosTol)
                    vote += o.w;
            if (vote > slotBest)
                slotBest = vote, bestIndex = i;
        }
        if (dirs.empty() || slotBest <= bestScore)
            continue;
        double sum[3] = {};
        for (const Dir& o : dirs)
        {
            const double c = dirs[bestIndex].d[0] * o.d[0] + dirs[bestIndex].d[1] * o.d[1] + dirs[bestIndex].d[2] * o.d[2];
            if (std::fabs(c) > cosTol)
                for (int q = 0; q < 3; q++)
                    sum[q] += (c > 0 ? 1 : -1) * o.d[q] * o.w;
        }
        const double len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        // vue verticale (pluie, occlusion) : pas le soleil
        if (len <= 0 || std::fabs(sum[1] / len) > 0.995)
            continue;
        bestScore = slotBest;
        bestSlot = s;
        for (int q = 0; q < 3; q++)
            bestSun[q] = sum[q] / len;
    }
    if (bestScore <= 0)
    {
        // ciel couvert, nuit, intérieur : pas de soleil, pas d'ombres (après trois essais de suite)
        if (++live.missedCalibrations >= 3 && live.calibrated)
        {
            live.calibrated = false;
            Log("URTI direct : plus de carte d'ombres du soleil, ombres RT suspendues");
        }
        else
            Log("URTI direct : étalonnage sans carte d'ombres du soleil");
        return;
    }
    live.missedCalibrations = 0;
    g_liveSunDepth = g.slotDepth[bestSlot];
    live.sunDirty = true;
    live.framesSinceSun = 0;
    // les directions par paire ont un sens défini (profondeur décroissante de la carte) ; le soleil est
    // au-dessus de l'horizon : ce choix fixe la convention du jeu pour le suivi image par image
    live.sunSign = bestSun[1] < 0 ? -1.0f : 1.0f;
    if (bestSun[1] < 0)
        for (double& v : bestSun)
            v = -v;
    for (int q = 0; q < 3; q++)
        live.sun[q] = float(bestSun[q]);
    if (!std::isfinite(A) || !std::isfinite(B) || B == 0)
        return; // loi dégénérée : rien n'est adopté
    live.A = float(A), live.B = float(B), live.p00 = p00, live.p11 = p11;
    g.depthReversed = B > 0; // profondeur inversée : d = B / w, le ciel (w infini) à 0
    live.calibrated = true;
    live.verified = true;
    Log("URTI direct : soleil (%.3f, %.3f, %.3f), profondeur = %.6f + %.6f / w", bestSun[0], bestSun[1], bestSun[2], A, B);
    if (ID3D12Resource* map = reinterpret_cast<ID3D12Resource*>(g_liveSunDepth))
    {
        const D3D12_RESOURCE_DESC d = map->GetDesc();
        const bool changed = live.fromStore && (std::fabs(live.A - float(A)) > 1e-4f * std::max(1.0f, std::fabs(float(A))) ||
                                                std::fabs(live.B - float(B)) > 1e-3f * std::fabs(float(B)) || live.sunMapWidth != uint32_t(d.Width) ||
                                                live.sunMapHeight != d.Height || live.sunMapFormat != uint32_t(d.Format));
        live.sunMapWidth = uint32_t(d.Width), live.sunMapHeight = d.Height, live.sunMapFormat = uint32_t(d.Format);
        LiveSaveCalibration();
        if (changed)
            Log("URTI direct : l'étalonnage stocké ne correspondait plus, il est remplacé");
    }
}

// ------------------------------------------------------------------------------------------------
// GPU : structure d'accélération et ombres, avant les effets de ReShade
// ------------------------------------------------------------------------------------------------

bool LiveCreatePipelines(ID3D12Device* device)
{
    auto make = [&](const void* code, size_t size, int srvCount, ComPtr<ID3D12RootSignature>& rs, ComPtr<ID3D12PipelineState>& pso, uint32_t constants,
                    int uavCount = 1) {
        D3D12_ROOT_PARAMETER p[16] = {}; // SRV (t0…), UAV (u0…), constantes
        for (int i = 0; i < srvCount; i++)
        {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[i].Descriptor.ShaderRegister = i;
        }
        for (int i = 0; i < uavCount; i++)
        {
            p[srvCount + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[srvCount + i].Descriptor.ShaderRegister = i;
        }
        p[srvCount + uavCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[srvCount + uavCount].Constants.Num32BitValues = constants;
        D3D12_ROOT_SIGNATURE_DESC d = { UINT(srvCount + uavCount + 1), p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(g_createRootSignature(device, 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = rs.Get();
        cd.CS = { code, size };
        return SUCCEEDED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso)));
    };
    return make(g_live_convert, sizeof(g_live_convert), 4, live.convertRs, live.convertPso, 8, 2) &&
           make(g_live_camfit, sizeof(g_live_camfit), 0, live.camfitRs, live.camfitPso, 4, 2) &&
           make(g_live_mvsynth, sizeof(g_live_mvsynth), 3, live.mvsynthRs, live.mvsynthPso, 8) &&
           make(g_live_planes, sizeof(g_live_planes), 2, live.planesRs, live.planesPso, 4, 2) &&
           make(g_live_shadow, sizeof(g_live_shadow), 9, live.shadowRs, live.shadowPso, 29, 3) &&
           make(g_live_mvcal, sizeof(g_live_mvcal), 4, live.mvcalRs, live.mvcalPso, 8) &&
           make(g_live_shadowtemporal, sizeof(g_live_shadowtemporal), 6, live.denoiseRs, live.denoisePso, 14) &&
           make(g_live_sunfit, sizeof(g_live_sunfit), 3, live.fitRs, live.fitPso, 4) &&
           make(g_live_sunvote, sizeof(g_live_sunvote), 1, live.voteRs, live.votePso, 8) &&
           make(g_live_composite, sizeof(g_live_composite), 11, live.compositeRs, live.compositePso, 25, 2) &&
           make(g_live_sunlight, sizeof(g_live_sunlight), 3, live.sunlightRs, live.sunlightPso, 20) &&
           make(g_live_counts, sizeof(g_live_counts), 2, live.countsRs, live.countsPso, 4) &&
           make(g_live_uicompare, sizeof(g_live_uicompare), 6, live.uiRs, live.uiPso, 8) &&
           make(g_live_uiselect, sizeof(g_live_uiselect), 5, live.uiSelRs, live.uiSelPso, 8, 2) &&
           make(g_live_reflresolve, sizeof(g_live_reflresolve), 9, live.resolveRs, live.resolvePso, 20) &&
           make(g_live_reflect, sizeof(g_live_reflect), 10, live.reflectRs, live.reflectPso, 20, 2) &&
           make(g_live_surfcache, sizeof(g_live_surfcache), 5, live.surfRs, live.surfPso, 18) &&
           make(g_live_regionstats, sizeof(g_live_regionstats), 3, live.statsRs, live.statsPso, 4) &&
           make(g_live_depthfit, sizeof(g_live_depthfit), 3, live.depthfitRs, live.depthfitPso, 4);
}

ComPtr<ID3D12Resource> UavBuffer(ID3D12Device* device, uint64_t size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

void Retire(ComPtr<ID3D12Resource>& r)
{
    if (r)
        live.retired[live.frame % 3].push_back(r);
    r.Reset();
}

// structure d'accélération assez grande pour `vertices` sommets
bool LiveEnsureCapacity(ID3D12Device5* device, uint64_t vertices)
{
    if (vertices <= live.capacity && live.blas)
        return true;
    const uint64_t cap = std::min<uint64_t>(kSlotBytes / 16, (vertices * 3 / 2 + 65535) / 3 * 3);
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexBuffer.StrideInBytes = 12;
    geom.Triangles.VertexCount = UINT(cap);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.NumDescs = 1;
    in.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    info.ScratchDataSizeInBytes = std::max(info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes);
    live.blasAge = ~0u; // nouvelle structure : construction complète
    Retire(live.view), Retire(live.blas), Retire(live.blasScratch);
    live.view = UavBuffer(device, cap * 12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Retire(live.viewPrev);
    live.viewPrev = UavBuffer(device, cap * 12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    live.prevValid = false;
    live.blas = UavBuffer(device, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    live.blasScratch = UavBuffer(device, info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.capacity = cap;
    if (!live.tlas)
    {
        D3D12_RAYTRACING_INSTANCE_DESC inst = {};
        inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
        inst.InstanceMask = 0xFF;
        live.instances = MakeBuffer(device, 2 * sizeof(inst), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin = {};
        tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tin.NumDescs = 2; // scène opaque (masque 1) et surfaces transparentes planes (masque 2)
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ti = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &ti);
        live.tlas = UavBuffer(device, ti.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        live.tlasScratch = UavBuffer(device, ti.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Log("URTI direct : structure d'accélération pour %llu sommets (%.1f Mo)", (unsigned long long)cap, info.ResultDataMaxSizeInBytes / 1048576.0);
    return live.view && live.blas && live.blasScratch && live.tlas && live.instances;
}

// images (profondeur, ombres) à la taille de la profondeur du jeu
// zone rendue : celle où le jeu dessine (zones de dessin de ses dessins à l'écran), sinon toute la profondeur.
// Avec le DLSS ou une résolution dynamique, des moteurs (Decima) dessinent dans un coin de cibles pleine taille :
// lues en entier, ombres et reflets tombaient décalés et étirés (×1,5 dans Horizon Forbidden West)
void LiveRenderArea(const D3D12_RESOURCE_DESC& depthDesc, uint32_t& w, uint32_t& h)
{
    w = uint32_t(depthDesc.Width), h = depthDesc.Height;
    const uint32_t vw = uint32_t(live.viewMax[0] + 0.5f), vh = uint32_t(live.viewMax[1] + 0.5f);
    if (live.renderAreaOn && vw >= 64 && vh >= 64 && vw <= w && vh <= h && (vw < w || vh < h))
        w = vw, h = vh;
}

bool LiveEnsureImages(effect_runtime* runtime, ID3D12Device* device, const D3D12_RESOURCE_DESC& depthDesc)
{
    uint32_t renderW = 0, renderH = 0;
    LiveRenderArea(depthDesc, renderW, renderH);
    if (live.mask && live.allocWidth == depthDesc.Width && live.allocHeight == depthDesc.Height && live.depthFormat == depthDesc.Format)
    {
        if (renderW != live.width || renderH != live.height)
            Log("URTI direct : zone rendue %ux%u dans la profondeur %llux%u", renderW, renderH, (unsigned long long)depthDesc.Width, depthDesc.Height);
        live.width = renderW, live.height = renderH; // les images (pleine taille) servent telles quelles
        return true;
    }
    Retire(live.depthBuf), Retire(live.shadowBuf), Retire(live.mask), Retire(live.prevShadowBuf);
    if (live.maskSrv.handle)
        runtime->get_device()->destroy_resource_view(live.maskSrv), live.maskSrv = {};
    live.allocWidth = uint32_t(depthDesc.Width), live.allocHeight = depthDesc.Height, live.depthFormat = depthDesc.Format;
    // images allouées à la taille pleine (une résolution dynamique change la zone rendue sans tout réallouer)
    live.width = live.allocWidth, live.height = live.allocHeight;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&depthDesc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    live.depthPitch = fp.Footprint.RowPitch;
    live.depthBuf = UavBuffer(device, total, D3D12_RESOURCE_STATE_COPY_DEST);
    live.shadowPitch = (live.width * 4 + 255) / 256 * 256;
    live.shadowBuf = UavBuffer(device, uint64_t(live.shadowPitch) * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.prevShadowBuf = UavBuffer(device, uint64_t(live.shadowPitch) * live.height, D3D12_RESOURCE_STATE_COPY_DEST);
    Retire(live.shadowRawBuf);
    live.shadowRawBuf = UavBuffer(device, uint64_t(live.shadowPitch) * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Retire(live.giRaw), Retire(live.giBuf), Retire(live.giPrev);
    live.giRaw = UavBuffer(device, 3ull * live.shadowPitch * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.giBuf = UavBuffer(device, 3ull * live.shadowPitch * live.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.giPrev = UavBuffer(device, 3ull * live.shadowPitch * live.height, D3D12_RESOURCE_STATE_COPY_DEST);
    live.giValid = false;
    Retire(live.depthPrevBuf);
    live.depthPrevBuf = UavBuffer(device, total, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = live.width;
    td.Height = live.height;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&live.mask))))
        return false;
    if (!runtime->get_device()->create_resource_view(resource{ reinterpret_cast<uint64_t>(live.mask.Get()) }, resource_usage::shader_resource,
                                                     resource_view_desc(format::r32_float), &live.maskSrv))
        return false;
    runtime->update_texture_bindings("URTI_SHADOW", live.maskSrv, live.maskSrv);
    for (auto& r : live.counterReadback)
        if (!r)
            r = MakeBuffer(device, kSlots * 8ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    live.width = renderW, live.height = renderH;
    if (renderW != live.allocWidth || renderH != live.allocHeight)
        Log("URTI direct : zone rendue %ux%u dans la profondeur %ux%u", renderW, renderH, live.allocWidth, live.allocHeight);
    return live.depthBuf && live.shadowBuf;
}

// tampons du suivi du soleil (créés une fois)
bool LiveEnsureSun(ID3D12Device* device)
{
    if (live.sunBuf)
        return true;
    for (auto& r : live.pairsUpload)
        r = MakeBuffer(device, kLiveMaxPairs * 48ull, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    for (auto& r : live.sunReadback)
        r = MakeBuffer(device, kLiveSunBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    live.pairDirs = UavBuffer(device, kLiveMaxPairs * 80ull, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.sunBuf = UavBuffer(device, kLiveSunBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    live.sunUpload = MakeBuffer(device, 48, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    return live.sunBuf && live.pairDirs && live.sunUpload && live.pairsUpload[2] && live.sunReadback[2];
}

FILETIME FileTime(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a))
        return {};
    return a.ftLastWriteTime;
}

bool Newer(const FILETIME& a, const FILETIME& b) { return CompareFileTime(&a, &b) > 0; }

#include "urti_nrd.inl" // débruiteurs de NVIDIA (si l'add-on est compilé avec NRD)

// toutes les 30 images : urti.ini modifié -> réglages relus ; shader recompilé dans urti_shaders -> pipeline
// recréé (l'ancien est libéré trois images plus tard)
void LiveHotReload(ID3D12Device* device)
{
    if (live.frame % 30 != 0)
        return;
    const std::string ini = g_dir + "\\urti.ini";
    if (const FILETIME t = FileTime(ini); Newer(t, live.iniTime))
    {
        const bool first = live.iniTime.dwLowDateTime == 0 && live.iniTime.dwHighDateTime == 0;
        live.iniTime = t;
        if (FILE* f = std::fopen(ini.c_str(), "r"))
        {
            // valeurs par défaut à chaque relecture : une ligne retirée du fichier rend son réglage d'origine
            float combine = -1.0f, mask = 0.0f, debug = 0.0f, gameBias = 0.0015f, rayBias = 0.003f, sunRadius = 0.5f;
            float refl = 1.0f, channel = 1, invert = 0, smoothMin = 0.5f, f0 = 0.04f, debugReflect = 0, slot = 2, everywhere = 0, rays = 2, debugView = -1;
            float reflAlphaMin = 0.1f, reflAlphaMax = 0.5f, reflGamma = 1.0f, reflDepthTol = 0.1f, anchorAlpha = 0.0f, anchorGamma = 2.5f, alphaScreen = 1, blasRefit = 0, persist = 1, jitter = 2, fovAuto = 1, sunMeasure = 1, gameDilate = 1, gameMin = 1, ao = 0.8f, aoRadius = 1.5f, sunShadows = 0, aoQuarter = 1, buildMs = 2.5f, surfCache = 1, aoMs = 2.5f, gi = 1.0f, personMask = 1, motionSource = 0, renderArea = 1, water = 1, aoRoughIni = 0, nrdOn = 0, nrdDisocclusion = 0.01f, nrdFrames = 5;
            float vertexBudget = 6000000, probeBudget = 1000000, slotLimit = 0, uiPhoto = 1; // budget : 6 M sur les 8 M d'un emplacement
            float zone[4] = {};
            char line[256];
            while (std::fgets(line, sizeof(line), f))
            {
                std::string l = line;
                l.erase(l.find_last_not_of(" \r\n\t") + 1);
                auto value = [&](const char* key, float& out) {
                    if (l.rfind(key, 0) == 0)
                        out = float(std::atof(l.c_str() + std::strlen(key)));
                };
                value("combine=", combine), value("show_mask=", mask), value("debug_game_shadow=", debug);
                value("game_bias=", gameBias), value("ray_bias=", rayBias), value("sun_radius=", sunRadius);
                value("reflect=", refl), value("reflect_channel=", channel), value("reflect_invert=", invert), value("smooth_min=", smoothMin);
                value("reflect_f0=", f0), value("debug_reflect=", debugReflect), value("material_slot=", slot), value("reflect_everywhere=", everywhere);
                value("shadow_rays=", rays), value("debug_view=", debugView);
                value("refl_alpha_min=", reflAlphaMin), value("refl_alpha_max=", reflAlphaMax);
                value("refl_gamma=", reflGamma), value("refl_depth_tol=", reflDepthTol);
                value("anchor_alpha=", anchorAlpha), value("anchor_gamma=", anchorGamma); // anchor_alpha=0 : ancrage coupé
                value("alpha_screen=", alphaScreen), value("blas_refit=", blasRefit); // blas_refit=0 : toujours reconstruire
                value("persist=", persist); // persist=0 : zones attribuées à la suite à chaque image (ancienne règle)
                // budget de géométrie (sommets ; 0 : sans budget), re-mesures par image, emplacement réduit (essai)
                value("vertex_budget=", vertexBudget), value("probe_budget=", probeBudget), value("slot_limit=", slotLimit), value("ui_photo=", uiPhoto);
                value("nrd=", nrdOn), value("nrd_disocclusion=", nrdDisocclusion), value("nrd_frames=", nrdFrames), value("jitter=", jitter), value("fov_auto=", fovAuto);
                value("sun_measure=", sunMeasure), value("game_dilate=", gameDilate), value("game_min=", gameMin), value("ao=", ao), value("ao_radius=", aoRadius);
                value("person_mask=", personMask), value("motion_source=", motionSource), value("render_area=", renderArea), value("water=", water), value("ao_rough=", aoRoughIni);
                if (l.rfind("ui_zone=", 0) == 0)
                    std::sscanf(l.c_str() + 8, "%f,%f,%f,%f", &zone[0], &zone[1], &zone[2], &zone[3]);
                value("shadows=", sunShadows), value("ao_quarter=", aoQuarter), value("build_ms=", buildMs), value("surf_cache=", surfCache), value("ao_ms=", aoMs), value("gi=", gi);
                float jl = 0;
                value("jitter_log=", jl);
                if (l.rfind("jitter_log=", 0) == 0)
                    live.jitterLog = jl != 0;
            }
            std::fclose(f);
            live.iniCombine = int(combine); // -1 : le mode vient du menu de ReShade (UrtiMode)
            g_liveShowMask = mask != 0, g_liveDebugGame = uint32_t(debug);
            g_liveGameBias = gameBias, g_liveRayBias = rayBias, g_liveSunRadius = sunRadius;
            live.reflStrength = refl, live.reflChannel = uint32_t(channel) & 3, live.reflInvert = invert != 0, live.smoothMin = smoothMin;
            live.f0 = f0, live.debugReflect = uint32_t(debugReflect), live.materialSlot = uint32_t(slot) & 7, live.reflEverywhere = everywhere != 0;
            live.shadowRays = std::clamp(uint32_t(rays), 1u, 4u);
            live.iniDebugView = int(debugView);
            live.reflAlphaMin = std::clamp(reflAlphaMin, 0.01f, 1.0f), live.reflAlphaMax = std::clamp(reflAlphaMax, live.reflAlphaMin, 1.0f);
            live.reflGamma = reflGamma, live.reflDepthTol = reflDepthTol, live.anchorAlpha = anchorAlpha, live.anchorGamma = anchorGamma;
            live.alphaScreen = alphaScreen != 0;
            live.blasRefit = uint32_t(std::max(0.0f, blasRefit));
            if (g_livePersist != (persist != 0))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_livePersist = persist != 0;
                g_livePlaces.clear(), g_liveFreeSpace.clear(), g_liveFreeCounts.clear(), g_livePlaceHigh = 0, g_liveNextCount = 0;
            }
            live.budgetMax = uint64_t(std::max(0.0f, vertexBudget)), live.probeBudget = uint64_t(std::max(0.0f, probeBudget));
            if (live.budgetMax == 0 || live.buildTarget <= 0 || live.vertexBudget == 0 || live.vertexBudget > live.budgetMax)
                live.vertexBudget = live.budgetMax;
            live.slotLimit = uint64_t(std::max(0.0f, slotLimit));
            live.uiPhoto = uiPhoto != 0;
            live.nrdShadows = nrdOn != 0, live.nrdDisocclusion = nrdDisocclusion, live.nrdFrames = nrdFrames;
            live.jitterMode = uint32_t(std::clamp(jitter, 0.0f, 2.0f));
            live.fovAuto = fovAuto != 0;
            live.sunMeasure = sunMeasure != 0;
            std::copy(std::begin(zone), std::end(zone), std::begin(live.uiZone));
            live.gameDilate = uint32_t(std::clamp(gameDilate, 0.0f, 3.0f));
            live.gameMin = gameMin != 0;
            live.aoAmount = std::clamp(ao, 0.0f, 1.0f), live.aoRadius = std::clamp(aoRadius, 0.1f, 10.0f);
            live.sunShadows = sunShadows != 0;
            live.aoQuarter = aoQuarter != 0, live.buildTarget = std::max(0.0f, buildMs), live.surfCacheOn = surfCache != 0;
            live.aoTarget = std::max(0.0f, aoMs);
            live.giStrength = std::clamp(gi, 0.0f, 4.0f);
            live.personMask = personMask != 0;
            live.renderAreaOn = renderArea != 0;
            live.waterOn = water != 0;
            live.aoRough = aoRoughIni != 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_liveTranspOn = live.waterOn;
            }
            live.motionSource = uint32_t(std::clamp(motionSource, 0.0f, 2.0f));
            if (live.motionSource == 2 && !live.mvSynthetic)
                live.mvSynthetic = true, live.mvConvention = 0;
            else if (live.motionSource == 1 && live.mvSynthetic)
                live.mvSynthetic = false, live.mvConvention = 255;
            if (live.aoTarget <= 0)
                live.aoRays = 4;
        }
        if (!first)
            Log("URTI direct : réglages relus (combine %d, masque %d, contrôle %u, game_bias %.4f, ray_bias %.4f, soleil %.2f° ; reflets %.2f, "
                "canal %u%s, lissé min %.2f, f0 %.3f, contrôle %u, cible %u)", live.iniCombine, g_liveShowMask, g_liveDebugGame, g_liveGameBias,
                g_liveRayBias, g_liveSunRadius, live.reflStrength, live.reflChannel, live.reflInvert ? " inversé" : "", live.smoothMin, live.f0,
                live.debugReflect, live.materialSlot);
    }
    struct Hot { const char* name; ID3D12RootSignature* rs; ComPtr<ID3D12PipelineState>* pso; };
    const Hot hot[5] = { { "live_shadow", live.shadowRs.Get(), std::addressof(live.shadowPso) },
                         { "live_composite", live.compositeRs.Get(), std::addressof(live.compositePso) },
                         { "live_reflect", live.reflectRs.Get(), std::addressof(live.reflectPso) },
                         { "live_surfcache", live.surfRs.Get(), std::addressof(live.surfPso) },
                         { "live_shadowtemporal", live.denoiseRs.Get(), std::addressof(live.denoisePso) } };
    for (int i = 0; i < 5; i++)
    {
        const std::string path = g_dir + "\\urti_shaders\\" + hot[i].name + ".cso";
        const FILETIME t = FileTime(path);
        if (!Newer(t, live.shaderTime[i]) || !hot[i].rs)
            continue;
        live.shaderTime[i] = t;
        std::vector<char> code;
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            code.resize(size_t(std::ftell(f)));
            std::fseek(f, 0, SEEK_SET);
            code.resize(std::fread(code.data(), 1, code.size(), f));
            std::fclose(f);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = hot[i].rs;
        cd.CS = { code.data(), code.size() };
        ComPtr<ID3D12PipelineState> pso;
        if (code.empty() || FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso))))
        {
            Log("URTI direct : shader %s refusé (signature ou code invalide), l'ancien reste", hot[i].name);
            continue;
        }
        live.retiredPso[live.frame % 3].push_back(*hot[i].pso);
        *hot[i].pso = pso;
        Log("URTI direct : shader %s rechargé à chaud", hot[i].name);
    }
}

// copie d'une cible du G-buffer (texels de 32 bits) dans un tampon, à la fin de son écriture (passage cible de
// rendu -> lecture), dans la liste de commandes du jeu
void LiveCopyGbuffer(command_list* cmd, uint64_t target, uint32_t count, const resource* resources, const resource_usage* oldStates,
                     const resource_usage* newStates, ComPtr<ID3D12Resource>& buf, uint64_t& bytes, uint32_t& pitch, uint32_t& width,
                     uint32_t& height, uint32_t& format, uint64_t& frame, const char* what)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (resources[i].handle != target)
            continue;
        const uint32_t before = PassD3D12State(static_cast<uint32_t>(oldStates[i])), after = PassD3D12State(static_cast<uint32_t>(newStates[i]));
        if (!(before & D3D12_RESOURCE_STATE_RENDER_TARGET) || (after & D3D12_RESOURCE_STATE_RENDER_TARGET))
            continue;
        ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(target);
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        if (PassTexelBytes(uint32_t(d.Format)) != 4 || d.SampleDesc.Count != 1)
            return;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!buf || bytes < total)
        {
            Retire(buf);
            buf = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
            bytes = total;
            Log("URTI direct : %s du G-buffer %llux%u format %d copié à chaque image", what, (unsigned long long)d.Width, d.Height, int(d.Format));
        }
        if (!buf)
            return;
        ID3D12GraphicsCommandList* list = Native(cmd);
        Transition(list, res, D3D12_RESOURCE_STATES(after), D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = buf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(after));
        pitch = fp.Footprint.RowPitch, width = uint32_t(d.Width), height = d.Height, format = uint32_t(d.Format), frame = live.frame;
        return;
    }
}

void LiveGbufferBarrier(command_list* cmd, uint32_t count, const resource* resources, const resource_usage* oldStates, const resource_usage* newStates)
{
    if (!g_liveCapture || !live.enabled || !OurDevice(cmd->get_device()))
        return;
    uint64_t target = 0, motionTarget = 0, albedoTarget = 0, normalTarget = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (live.materialSlot < g_liveGbufferCount && live.reflStrength > 0)
            target = g_liveGbuffer[live.materialSlot];
        // couleur propre des surfaces (première cible du G-buffer) : rebond de lumière
        if (g_liveGbufferCount >= 3 && live.giStrength > 0 && live.aoAmount > 0)
            albedoTarget = g_liveGbuffer[0], normalTarget = live.personMask ? g_liveGbuffer[1] : 0;
        // vecteurs de mouvement : une cible du G-buffer en deux flottants 16 bits (la n-ième s'il y en a plusieurs)
        uint64_t candidates[8] = {};
        uint32_t found = 0;
        for (uint32_t k = 0; k < g_liveGbufferCount && k < 8; k++)
            if (g_liveGbuffer[k])
            {
                const DXGI_FORMAT f = reinterpret_cast<ID3D12Resource*>(g_liveGbuffer[k])->GetDesc().Format;
                if (f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_TYPELESS)
                    candidates[found++] = g_liveGbuffer[k];
            }
        if (found && !live.mvSynthetic)
            motionTarget = candidates[live.mvCandidate % found];
    }
    for (uint32_t i = 0; i < count && motionTarget; i++)
    {
        if (resources[i].handle != motionTarget)
            continue;
        const uint32_t before = PassD3D12State(static_cast<uint32_t>(oldStates[i])), after = PassD3D12State(static_cast<uint32_t>(newStates[i]));
        if (!(before & D3D12_RESOURCE_STATE_RENDER_TARGET) || (after & D3D12_RESOURCE_STATE_RENDER_TARGET))
            continue;
        ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(motionTarget);
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!live.mvBuf || live.mvBytes < total)
        {
            Retire(live.mvBuf);
            live.mvBuf = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
            live.mvBytes = total;
            Log("URTI direct : vecteurs de mouvement du jeu %llux%u copiés à chaque image (accumulation dans le temps)", (unsigned long long)d.Width, d.Height);
        }
        if (!live.mvBuf)
            break;
        ID3D12GraphicsCommandList* l = Native(cmd);
        Transition(l, res, D3D12_RESOURCE_STATES(after), D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = live.mvBuf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(l, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(after));
        live.mvPitch = fp.Footprint.RowPitch, live.mvWidth = uint32_t(d.Width), live.mvHeight = d.Height, live.mvFrame = live.frame;
        break;
    }
    if (target)
        LiveCopyGbuffer(cmd, target, count, resources, oldStates, newStates, live.materialBuf, live.materialBytes, live.materialPitch,
                        live.materialWidth, live.materialHeight, live.materialFormat, live.materialFrame, "matériau (cible 2)");
    if (albedoTarget)
        LiveCopyGbuffer(cmd, albedoTarget, count, resources, oldStates, newStates, live.albedoBuf, live.albedoBytes, live.albedoPitch,
                        live.albedoWidth, live.albedoHeight, live.albedoFormat, live.albedoFrame, "couleur propre (albedo, cible 0)");
    if (normalTarget)
        LiveCopyGbuffer(cmd, normalTarget, count, resources, oldStates, newStates, live.normalBuf, live.normalBytes, live.normalPitch,
                        live.normalWidth, live.normalHeight, live.normalFormat, live.normalFrame, "normale (cible 1)");
}

// horodatage k (0 à 7) de l'image en cours
void LiveTime(ID3D12GraphicsCommandList* list, uint32_t k)
{
    if (live.timeHeap)
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (live.frame % 3) * 8 + k);
}

int LiveMainSlot()
{
    int best = -1;
    uint32_t most = 0;
    for (uint32_t s = 0; s < kSlots; s++)
        if (g.slotDepth[s] && g.screenLike[g.slotDepth[s]] == 1 && g.slotDraws[s] >= most)
            most = g.slotDraws[s], best = int(s);
    return best;
}

void OnBeginEffects(effect_runtime* runtime, command_list* cmd, resource_view, resource_view)
{
    if (!OurDevice(runtime->get_device()))
        return;
    if (live.toggleSplit.exchange(false))
        if (const effect_uniform_variable split = runtime->find_uniform_variable("urti.fx", "UrtiSplit"); split.handle)
        {
            bool on = false;
            runtime->get_uniform_value_bool(split, &on, 1);
            runtime->set_uniform_value_bool(split, !on);
            Log("URTI : écran partagé %s", on ? "coupé" : "actif");
        }
    if (!live.enabled)
    {
        // ombres coupées : l'effet ne doit plus appliquer l'ancienne image d'ombres
        if (const effect_uniform_variable off = runtime->find_uniform_variable("urti.fx", "UrtiActive"); off.handle)
            runtime->set_uniform_value_bool(off, false);
        live.bound = false;
        return;
    }
    if (g.phase != Phase::Idle)
        return;
    live.statEffects++;
    ID3D12Device5* device5 = nullptr;
    if (FAILED(g.device->QueryInterface(IID_PPV_ARGS(&device5))))
        return;
    ComPtr<ID3D12Device5> device(device5);
    device5->Release();
    ComPtr<ID3D12GraphicsCommandList4> list;
    if (FAILED(Native(cmd)->QueryInterface(IID_PPV_ARGS(&list))))
        return;
    live.retired[live.frame % 3].clear();
    live.retiredPso[live.frame % 3].clear();
    if (!live.convertPso && !LiveCreatePipelines(g.device))
    {
        Log("URTI direct : shaders refusés, mode direct coupé");
        live.enabled = false;
        return;
    }
    LiveHotReload(g.device);
    if (live.iniCombine >= 0)
        g_liveCombine = live.iniCombine != 0;
    else
        g_liveCombine = true; // seulement les ombres que le jeu n'a pas (le menu n'a plus que « URTI activé »)

    // ce que le flux a reçu il y a trois images (lecture sans attente)
    if (const uint64_t* c = nullptr; live.counterReadback[live.frame % 3] && SUCCEEDED(live.counterReadback[live.frame % 3]->Map(0, nullptr, (void**)&c)) && c)
    {
        const int slot = LiveMainSlot();
        if (slot >= 0)
            live.lastFilled = c[slot] / 16;
        live.counterReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    const int slot = LiveMainSlot();
    const uint32_t drawsThisFrame = slot >= 0 ? uint32_t(g.slotDraws[slot]) : 0;
    std::vector<LiveMark> marksNow;
    std::vector<LiveRegion> regionsNow, probesNow, placesNow;
    // début dans le flux (octets) de la zone écrite par chaque dessin de cette image, dans l'ordre de regionsNow ;
    // de chaque place, dans l'ordre de placesNow (place gardée sans être dessinée : pas lue)
    std::vector<uint32_t> srcNow, placeSrc;
    std::vector<LiveRegion> transpNow; // surfaces transparentes exécutées (couche 2) et leur début dans le flux
    std::vector<uint32_t> transpSrcNow;
    uint32_t transpCursorNow = 0;
    uint32_t cursorNow = 0, zeroArea = ~0u;
    bool persistNow = false;
    if (live.lastPresentDraws == 0 && live.bound)
    {
        // aucun dessin du jeu exécuté depuis la présentation précédente (image générée par DLSS-G ou FSR3, ou jeu
        // qui présente deux fois) : rien de neuf à calculer, le dernier résultat est reposé ; la fenêtre de capture
        // n'avance pas (les dessins exécutés ensuite appartiennent à l'image suivante du jeu)
        live.statNoDraw++;
        live.frame++;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        marksNow.swap(g_liveMarks);
        {
            // la plus grande des zones assez fréquentes (un cinquième des dessins de la plus fréquente) : des passes à
            // mi-résolution (852×480 dans Horizon) dessinent aussi dans cette profondeur, quelques repères en 3D
            // gardent la zone pleine
            uint32_t most = 0;
            for (const auto& [extent, n] : g_liveViewCounts)
                most = std::max(most, n);
            uint32_t best = 0;
            uint64_t bestArea = 0;
            for (const auto& [extent, n] : g_liveViewCounts)
                if (n * 5 >= most && uint64_t(extent >> 16) * (extent & 0xFFFF) > bestArea)
                    best = extent, bestArea = uint64_t(extent >> 16) * (extent & 0xFFFF);
            // une image de temps en temps (sonde d'environnement ?) est dominée par une autre zone : la zone rendue ne
            // change qu'après dix images de suite avec la même nouvelle zone (une résolution dynamique change lentement)
            if (most >= 16)
            {
                if (best == live.viewCandidate)
                    live.viewStreak++;
                else
                    live.viewCandidate = best, live.viewStreak = 1;
                if (live.viewStreak >= 10 || live.viewMax[0] == 0)
                    live.viewMax[0] = float(best >> 16), live.viewMax[1] = float(best & 0xFFFF);
            }
            if (++live.viewLogCount % 600 == 1 && !g_liveViewCounts.empty())
            {
                std::vector<std::pair<uint32_t, uint32_t>> v(g_liveViewCounts.begin(), g_liveViewCounts.end());
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                char text[256] = "";
                for (size_t i = 0; i < v.size() && i < 5; i++)
                {
                    char item[48];
                    std::snprintf(item, sizeof(item), " %ux%u : %u", v[i].first >> 16, v[i].first & 0xFFFF, v[i].second);
                    std::strncat(text, item, sizeof(text) - std::strlen(text) - 1);
                }
                Log("URTI direct : zones de dessin dans la profondeur de l'écran (dessins) :%s", text);
            }
            g_liveViewCounts.clear();
        }
        if (slot >= 0)
            regionsNow.swap(g_liveRegions[slot]), srcNow.swap(g_liveRegionSrc[slot]), cursorNow = g_liveCursor[slot];
        transpNow.swap(g_liveTranspRegions), transpSrcNow.swap(g_liveTranspSrc), transpCursorNow = g_liveTranspCursor;
        g_liveTranspRegions.clear(), g_liveTranspSrc.clear(), g_liveTranspCursor = 0, g_liveTranspVerts = 0;
        // places fixes : un objet absent depuis plus de kLiveKeepFrames images rend sa place ; les autres restent
        // dans la scène RT avec leur dernière géométrie ; partie occupée trop morcelée : tout est redistribué
        if (g_livePersist && slot >= 0 && g.slotDepth[slot] == g_liveMainDepth)
        {
            constexpr uint64_t kLiveKeepFrames = 3;
            uint64_t held = 0;
            for (auto it = g_livePlaces.begin(); it != g_livePlaces.end();)
            {
                if (g_livePlaceFrame - it->second.lastFrame > kLiveKeepFrames)
                {
                    LiveFreePlace(it->second);
                    it = g_livePlaces.erase(it);
                    continue;
                }
                // dessinée dans cette image : convertie depuis sa zone du flux, jusqu'à son compteur ; sinon gardée
                // telle quelle dans la scène RT (compteur ~0 : la conversion n'y touche pas)
                const bool drawn = it->second.lastFrame == g_livePlaceFrame;
                placesNow.push_back({ it->second.offset, it->second.capacity, drawn ? it->second.frameCounter : ~0u, it->first });
                held += it->second.capacity;
                ++it;
            }
            std::sort(placesNow.begin(), placesNow.end(), [](const LiveRegion& a, const LiveRegion& b) { return a.offset < b.offset; });
            {
                std::vector<uint32_t> order(regionsNow.size());
                for (uint32_t i = 0; i < order.size(); i++)
                    order[i] = i;
                std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return regionsNow[a].offset < regionsNow[b].offset; });
                std::vector<LiveRegion> r(order.size());
                std::vector<uint32_t> src(order.size());
                for (size_t i = 0; i < order.size(); i++)
                    r[i] = regionsNow[order[i]], src[i] = srcNow[order[i]];
                regionsNow.swap(r), srcNow.swap(src);
            }
            std::unordered_map<uint32_t, uint32_t> srcOf;
            for (size_t i = 0; i < regionsNow.size(); i++)
                srcOf[regionsNow[i].id] = srcNow[i];
            placeSrc.resize(placesNow.size());
            for (size_t i = 0; i < placesNow.size(); i++)
            {
                const auto f = srcOf.find(placesNow[i].id);
                placeSrc[i] = f != srcOf.end() ? f->second : 0;
                // dessinée à la même place à l'image précédente : ses anciens sommets servent au mouvement de la caméra
                if (f != srcOf.end())
                    if (const auto pl = g_livePlaces.find(placesNow[i].id);
                        pl != g_livePlaces.end() && pl->second.prevFrame + 1 == g_livePlaceFrame && pl->second.stableOrder && pl->second.uniquePrev &&
                        g_liveKeyRank[pl->second.rankKey] == 1)
                        placeSrc[i] |= 0x80000000u;
            }
            cursorNow = g_livePlaceHigh, persistNow = true;
            live.statPlaces += g_livePlaces.size(), live.statPlaceHeld += held, live.statPlaceHigh += g_livePlaceHigh;
            live.statPlaceNew += g_livePlaceNew, live.statPlaceFrames++;
            live.placesMoved = live.placesResetPending || g_livePlaceNew > placesNow.size() / 16 + 8;
            live.placesResetPending = false;
            // une place refusée (emplacement plein), ou plus d'un cinquième de la partie occupée perdu en trous :
            // toutes les places sont rendues, l'image suivante les redistribue à la suite (la structure étant
            // reconstruite à chaque image, seuls les objets gardés sans être dessinés manquent une image)
            if (g_livePlaceFailed || held < uint64_t(g_livePlaceHigh) * 8 / 10)
            {
                live.statPlaceResets++, live.placesResetPending = true;
                g_livePlaces.clear(), g_liveFreeSpace.clear(), g_liveFreeCounts.clear(), g_livePlaceHigh = 0, g_liveNextCount = 0;
            }
            g_livePlaceNew = g_livePlaceFailed = 0;
        }
        else if (!g_livePlaces.empty())
            g_livePlaces.clear(), g_liveFreeSpace.clear(), g_liveFreeCounts.clear(), g_livePlaceHigh = 0, g_liveNextCount = 0;
        g_livePlaceFrame++;
        for (uint32_t s = 0; s < kSlots; s++)
            g_liveRegions[s].clear(), g_liveRegionSrc[s].clear(), g_liveCursor[s] = 0;
        // maillages dessinés une seule fois dans cette image (pour les paires de l'image suivante)
        for (auto& [id, place] : g_livePlaces)
            if (place.lastFrame == g_livePlaceFrame - 1)
            {
                const auto r = g_liveKeyRank.find(place.rankKey);
                place.uniquePrev = r != g_liveKeyRank.end() && r->second == 1;
            }
        g_liveKeyRank.clear();
        if (slot >= 0)
            probesNow.swap(g_liveProbes[slot]);
        for (uint32_t s = 0; s < kSlots; s++)
            g_liveProbes[s].clear();
        // fenêtre suivante : sa partie du flux (servie il y a trois fenêtres) repart du début ; ses compteurs sont
        // remis à zéro à la fin de cette image, avant toute liste écrite dans la nouvelle fenêtre
        g_liveWindow++;
        zeroArea = uint32_t(g_liveWindow % kLiveAreas);
        for (uint32_t s = 0; s < kSlots; s++)
            g_liveAreaCursor[s][zeroArea] = g_liveAreaProbe[s][zeroArea] = 0;
        g_liveAreaCounters[zeroArea] = 0;
        for (auto it = g_livePending.begin(); it != g_livePending.end();) // listes écrites mais jamais exécutées
            it = !it->second.empty() && g_liveWindow - it->second.front().window > 8 ? g_livePending.erase(it) : std::next(it);
        for (const auto& [id, mesh] : g_liveIdMesh)
            live.idMesh[id] = mesh;
        g_liveIdMesh.clear();
        if (live.idMesh.size() > 200000)
            live.idMesh.clear();
        live.seenNow.swap(g_liveSeen);
        for (const auto& [id, sig] : g_liveIdSig)
            live.idSig[id] = sig;
        g_liveIdSig.clear();
        if (live.idSig.size() > 50000)
            live.idSig.clear();
        g_liveSeen.clear();
        live.statOverflow += g_liveOverflow, live.statOutOfBudget += g_liveOutOfBudget, live.statReserved += g_liveReserved;
        g_liveOverflow = g_liveOutOfBudget = 0, g_liveReserved = 0, g_liveProbeVerts = 0;
        if (!g_liveSunKeys.empty())
            g_livePrevSunKeys.swap(g_liveSunKeys);
        g_liveSunKeys.clear();
        g_liveSunMarked = 0;
    }
    // compteurs remis à zéro dans la liste de cette image (avant toute liste écrite dans la fenêtre suivante) : ceux
    // des emplacements (étalonnage) et ceux de la partie du flux de la fenêtre suivante, après leur dernière lecture
    auto zeroCounters = [&](D3D12_RESOURCE_STATES from) {
        Transition(list.Get(), g.counters.Get(), from, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(g.counters.Get(), 0, g.zeros.Get(), 0, kSlots * 8ull);
        if (zeroArea != ~0u)
            list->CopyBufferRegion(g.counters.Get(), (kSlots + uint64_t(zeroArea) * kLiveAreaCounters) * 8ull, g.zeros.Get(), 0, uint64_t(kLiveAreaCounters) * 8ull);
        Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    };
    // budget de géométrie : mesures arrivées (trois images plus tard), puis choix des objets de la scène RT
    // pour les images suivantes : par taille apparente décroissante jusqu'au budget (bonus aux objets déjà
    // dedans : pas de va-et-vient au bord du budget) ; les objets inconnus entrent d'office (mesurés ensuite)
    {
        std::vector<uint32_t>& ids = live.statsIds[live.frame % 3];
        if (!ids.empty() && live.statsReadback[live.frame % 3])
        {
            const float* v = nullptr;
            if (SUCCEEDED(live.statsReadback[live.frame % 3]->Map(0, nullptr, (void**)&v)) && v)
            {
                for (size_t i = 0; i < ids.size(); i++)
                {
                    const float minZ = v[i * 4], size = v[i * 4 + 1];
                    const float importance = (minZ < 1e8f && size > 0) ? size / std::max(minZ, 0.3f) : 0.0f;
                    live.objects[ids[i]] = { importance, live.frame, uint32_t(v[i * 4 + 2]) };
                    // ExecuteIndirect : une zone remplie jusqu'au bout a été tronquée : réserve maximale à l'image suivante
                    if (live.idSig.count(ids[i]))
                    {
                        const std::vector<uint32_t>& caps = live.statsCaps[live.frame % 3];
                        const float written = v[i * 4 + 2], cap = i < caps.size() ? float(caps[i]) : 0.0f;
                        if (cap > 0 && written >= cap)
                            live.objects[ids[i]].vertices = 65536; // la mesure suivante dira la vraie taille
                    }
                }
                live.statsReadback[live.frame % 3]->Unmap(0, nullptr);
            }
            ids.clear();
            live.statsCaps[live.frame % 3].clear();
        }
        if (live.objects.size() > 200000)
            live.objects.clear();
        // budget par maillage (tous ses exemplaires ensemble) : l'identité d'un exemplaire n'est connue qu'à
        // l'exécution, le choix de capter se fait à l'écriture. Mesures des exemplaires réunies : importance du plus
        // important, sommets écrits du plus gros, mesure la plus ancienne
        struct MeshInfo
        {
            float importance = 0;
            uint32_t vertices = 0;
            uint64_t frame = ~0ull;
        };
        std::unordered_map<uint32_t, MeshInfo> meshes;
        for (const auto& [id, o] : live.objects)
            if (const auto m = live.idMesh.find(id); m != live.idMesh.end())
            {
                MeshInfo& mi = meshes[m->second];
                mi.importance = std::max(mi.importance, o.importance);
                mi.vertices = std::max(mi.vertices, o.vertices);
                mi.frame = std::min(mi.frame, o.frame);
            }
        std::unordered_set<uint32_t> known, admit, probe;
        if (live.vertexBudget > 0)
        {
            std::vector<std::pair<float, uint32_t>> ranked;
            uint64_t used = 0;
            for (const auto& [mesh, vertices] : live.seenNow)
            {
                const auto it = meshes.find(mesh);
                if (it == meshes.end())
                {
                    used += vertices; // inconnu : entre d'office
                    continue;
                }
                known.insert(mesh);
                ranked.push_back({ it->second.importance * (live.admitted.count(mesh) ? 1.3f : 1.0f), mesh });
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<std::pair<uint64_t, uint32_t>> stale;
            // un maillage trop gros pour la place restante est sauté, les suivants peuvent entrer ; sommets comptés :
            // ceux réservés par tous ses exemplaires dans cette image
            for (const auto& [score, mesh] : ranked)
            {
                const uint32_t vertices = live.seenNow[mesh];
                if (used + vertices <= live.vertexBudget)
                    used += vertices, admit.insert(mesh);
                else
                    stale.push_back({ meshes[mesh].frame, mesh });
            }
            // re-mesures : les mesures les plus anciennes d'abord
            std::sort(stale.begin(), stale.end());
            uint64_t probed = 0;
            for (const auto& [frame, mesh] : stale)
            {
                const uint32_t vertices = live.seenNow[mesh];
                if (probed + vertices > live.probeBudget)
                    continue;
                probed += vertices, probe.insert(mesh);
            }
        }
        if (live.frame % 300 == 150)
        {
            uint64_t unknownVerts = 0, knownVerts = 0;
            for (const auto& [mesh, vertices] : live.seenNow)
                (known.count(mesh) ? knownVerts : unknownVerts) += vertices;
            Log("URTI direct : budget : %zu maillages à l'écran (%llu sommets mesurés, %llu pas encore mesurés), %zu dans la scène RT, %zu exemplaires mesurés",
                live.seenNow.size(), (unsigned long long)knownVerts, (unsigned long long)unknownVerts, admit.size(), live.objects.size());
            // les plus gros objets : sommets réservés (nombre d'indices × instances) et réellement écrits
            std::vector<std::pair<uint32_t, uint32_t>> big;
            for (const auto& [id, vertices] : live.seenNow)
                big.push_back({ vertices, id });
            std::sort(big.begin(), big.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            uint64_t admittedReserved = 0;
            for (uint32_t mesh : admit)
                admittedReserved += live.seenNow[mesh];
            char top[256] = "";
            for (size_t i = 0; i < big.size() && i < 5; i++)
            {
                const auto it = meshes.find(big[i].second);
                char item[48];
                std::snprintf(item, sizeof(item), " %u/%u", big[i].first, it != meshes.end() ? it->second.vertices : 0u);
                std::strncat(top, item, sizeof(top) - std::strlen(top) - 1);
            }
            Log("URTI direct : budget : sommets réservés des maillages admis %llu ; plus gros maillages (réservés, tous exemplaires / plus gros exemplaire écrit) :%s",
                (unsigned long long)admittedReserved, top);
        }
        live.churnIn = live.churnOut = 0;
        for (const uint32_t id : admit)
            live.churnIn += live.admitted.count(id) ? 0 : 1;
        for (const uint32_t id : live.admitted)
            live.churnOut += admit.count(id) ? 0 : 1;
        live.statChurn += live.churnIn + live.churnOut;
        live.admitted = admit;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveKnown.swap(known), g_liveAdmit.swap(admit), g_liveProbeWant.swap(probe);
        g_liveBudget = live.vertexBudget > 0;
        g_liveBudgetVertices = live.vertexBudget, g_liveProbeBudget = live.probeBudget;
        g_liveSlotLimit = live.slotLimit;
        g_liveMainDepth = slot >= 0 ? g.slotDepth[slot] : 0;
        // tailles apprises des ExecuteIndirect : par signature de commande (à chaque image), et par identité
        if (live.frame % 10 == 0)
        {
            g_liveLearned.clear();
            for (const auto& [id, o] : live.objects)
                if (live.frame - o.frame < 120) // 0 compris : un ExecuteIndirect qui ne dessine rien ne réserve presque rien
                    if (const auto m = live.idMesh.find(id); m != live.idMesh.end())
                    {
                        const auto [jt, added] = g_liveLearned.emplace(m->second, o.vertices);
                        if (!added)
                            jt->second = std::max(jt->second, o.vertices);
                    }
        }
    }
    // mouvement de la caméra mesuré sur notre géométrie, lu trois images plus tard
    if (const float* m = nullptr; live.motionReadback[live.frame % 3] && SUCCEEDED(live.motionReadback[live.frame % 3]->Map(0, nullptr, (void**)&m)) && m)
    {
        live.motFrames++;
        if (m[12] > 0.5f)
        {
            const double tr = std::clamp((double(m[0]) + m[4] + m[8] - 1.0) * 0.5, -1.0, 1.0);
            const double rot = std::acos(tr) * 180.0 / 3.14159265, move = std::sqrt(double(m[9]) * m[9] + double(m[10]) * m[10] + double(m[11]) * m[11]);
            live.motValid++, live.motRot += rot, live.motTrans += move;
            live.motKept += m[14] > 0 ? m[13] / m[14] : 0.0, live.motRms += m[15];
            // la caméra bouge (notre mesure) et les vecteurs du jeu ne se laissent toujours pas lire : ils ne
            // contiennent sans doute pas le mouvement de la caméra (seulement celui des personnages) ; trois secondes
            // ainsi et les nôtres les remplacent
            if (live.motionSource == 0 && !live.mvSynthetic && live.mvConvention == 255 && (rot > 0.05 || move > 0.005) && ++live.motMovingNoGame >= 180)
            {
                live.mvSynthetic = true, live.mvConvention = 0;
                Log("URTI direct : la caméra bouge et les vecteurs de mouvement du jeu restent illisibles : mouvement de la caméra mesuré sur notre "
                    "géométrie à la place");
                if (live.verified || live.fromStore)
                    LiveSaveCalibration();
            }
        }
        live.motionReadback[live.frame % 3]->Unmap(0, nullptr);
        if (live.motFrames >= 300)
        {
            const double v = std::max(1u, live.motValid);
            Log("URTI direct : mouvement de la caméra mesuré sur notre géométrie : %u images sur %u, rotation %.3f° et translation %.1f cm par image en "
                "moyenne, %.0f %% des paires gardées, écart %.1f mm%s", live.motValid, live.motFrames, live.motRot / v, live.motTrans / v * 100.0,
                live.motKept / v * 100.0, live.motRms / v * 1000.0, live.mvSynthetic ? " (vecteurs de mouvement fabriqués avec lui)" : "");
            live.motFrames = live.motValid = 0, live.motRot = live.motTrans = live.motKept = live.motRms = 0;
        }
    }
    // bilan du suivi du soleil, lu trois images plus tard
    if (const float* r = nullptr; live.sunReadback[live.frame % 3] && SUCCEEDED(live.sunReadback[live.frame % 3]->Map(0, nullptr, (void**)&r)) && r)
    {
        if (live.frame % 600 == 0)
            Log("URTI direct : loi de profondeur de l'image : %g + %g / w (valide %.0f) ; stockée : %g + %g / w", r[9], r[10], r[11], live.A, live.B);
        if (live.calibrated)
        {
            live.sunFrames++, live.pairSum += uint64_t(r[4]);
            if (live.jitterLog) // urti.ini jitter_log=1 : décalage sous-pixel mesuré à chaque image (celle d'il y a trois)
                Log("JITTER %llu valide %.0f ecart %.6f %.6f absolu %.6f %.6f s2 %.3f t2 %.3f", (unsigned long long)live.frame, r[156], r[157], r[158], r[296], r[297], r[304], r[305]);
            if (r[156] > 0) // rotation de la caméra mesurée (octet 624)
            {
                const double tr = r[144] + r[149] + r[154]; // trace de la matrice (lignes à 576, 592, 608)
                live.rotationSum += std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0)) * 180.0 / 3.14159265;
                live.rotationValid++;
            }
            if (r[7] > 0)
            {
                const float dot = r[0] * live.lastSun[0] + r[1] * live.lastSun[1] + r[2] * live.lastSun[2];
                if (live.framesSinceSun == 0 && dot > 0)
                {
                    const double deg = std::acos(std::min(1.0, double(dot))) * 180.0 / 3.14159265;
                    live.sunJitterSum += deg, live.sunJitterMax = std::max(live.sunJitterMax, deg), live.sunJitterCount++;
                }
                live.lastSun[0] = r[0], live.lastSun[1] = r[1], live.lastSun[2] = r[2];
                live.sunAccepted++, live.framesSinceSun = 0;
                if (!live.verified)
                {
                    live.verified = true;
                    live.forceCalibration = true; // vérification discrète de l'étalonnage stocké
                }
            }
            else
                live.framesSinceSun++;
            if (r[305] > 0)
                live.fovStretch = r[305]; // t² : (tan du demi-champ supposé / tan du vrai)²
            // part du soleil dans l'image du jeu : médiane de chaque histogramme (live_sunlight), lissée en log
            // (une demi-seconde environ) ; trop peu de paires (pas d'ombre du jeu à l'écran) : valeur gardée
            const uint32_t* check = reinterpret_cast<const uint32_t*>(r) + 472; // octet 1888
            for (int k = 0; k < 6; k++)
                live.mapCheck[k] += check[k];
            for (int k = 0; k < 16; k++)
                live.depthHist[k] += check[8 + k], live.gapHist[k] += check[24 + k];
            // mesure acceptée seulement si la carte lue concorde avec l'image : les points qu'elle éclaire sont
            // nettement plus clairs que ceux qu'elle ombre (sinon elle est mal lue : la valeur d'avant reste)
            const bool mapAgrees = check[0] >= 32 && check[1] >= 32 && check[5] > 0 &&
                                   (double(check[4]) / check[0]) > 1.3 * (double(check[5]) / check[1]);
            if (const uint32_t* u = reinterpret_cast<const uint32_t*>(r); live.sunMeasure && mapAgrees && u[308] >= 32)
            {
                float measured[4] = {};
                for (int c = 0; c < 4; c++)
                {
                    const uint32_t* h = u + 312 + c * 40;
                    uint32_t total = 0, below = 0;
                    for (int b = 0; b < 40; b++)
                        total += h[b];
                    int b = 0;
                    while (b < 39 && below + h[b] < total / 2)
                        below += h[b++];
                    const float f = h[b] ? std::clamp((total * 0.5f - below) / h[b], 0.0f, 1.0f) : 0.5f;
                    measured[c] = std::exp2((b + f) / 4.0f - 5.0f);
                }
                for (int c = 0; c < 4; c++)
                {
                    const float old = std::log2(live.sunSky[c]), now = std::log2(measured[c]);
                    live.sunSky[c] = std::exp2(live.sunSkyFrames ? old + (now - old) * 0.05f : now);
                }
                live.sunSkyFrames++, live.sunSkyPairs += u[308];
            }
            // champ de vision du jeu : la projection d'URTI est corrigée quand la mesure est fiable (assez de
            // rotation de la caméra) et s'écarte de plus de 1,5 % ; les reflets ont alors des angles exacts (dans
            // un espace caméra étiré, la normale du sol est fausse et le reflet décalé : il « flotte »)
            if (live.fovHold)
                live.fovHold--;
            else if (live.fovAuto && r[304] > 0 && r[305] > 0 && r[300] + r[302] > 4e-4f)
            {
                const float k2 = (r[300] * r[304] + r[302] * r[305]) / (r[300] + r[302]);
                if (std::fabs(k2 - 1.0f) > 0.03f)
                {
                    const float before = 2.0f * std::atan(1.0f / live.p11) * 57.2957795f;
                    const float aspect = live.p11 / live.p00;
                    live.p11 = std::clamp(live.p11 * std::pow(k2, 0.4f), // 80 % de l'écart : pas d'oscillation
                                            1.0f / std::tan(55.0f / 57.2957795f), 1.0f / std::tan(10.0f / 57.2957795f));
                    live.p00 = live.p11 / aspect;
                    live.fovReset = true, live.fovHold = 6;
                    Log("URTI direct : champ de vision corrigé : %.1f° -> %.1f°", before, 2.0f * std::atan(1.0f / live.p11) * 57.2957795f);
                }
            }
            if (live.sunFrames == 300)
            {
                if (live.fovStretch > 0 && live.p11 > 0)
                    Log("URTI direct : champ de vision vertical du jeu mesuré : %.1f° (URTI suppose %.1f°)",
                        2.0 * std::atan(1.0 / (live.p11 * std::sqrt(double(live.fovStretch)))) * 180.0 / 3.14159265,
                        2.0 * std::atan(1.0 / double(live.p11)) * 180.0 / 3.14159265);
                Log("URTI direct : soleil suivi (%.3f, %.3f, %.3f), mesuré sur %.0f %% des images, %.0f paires par image, %u cascades", r[0], r[1], r[2],
                    100.0 * live.sunAccepted / live.sunFrames, double(live.pairSum) / live.sunFrames, reinterpret_cast<const uint32_t*>(r)[8]);
                if (live.sunJitterCount)
                    Log("URTI direct : soleil d'une image à l'autre : %.3f° en moyenne, %.2f° au plus (caméra comprise)", live.sunJitterSum / live.sunJitterCount,
                        live.sunJitterMax);
                live.sunJitterSum = live.sunJitterMax = 0, live.sunJitterCount = 0;
                if (live.rotationValid)
                    Log("URTI direct : rotation de la caméra mesurée sur %u images sur 300 (cascades), %.3f° par image en moyenne", live.rotationValid,
                        live.rotationSum / live.rotationValid);
                live.rotationValid = 0, live.rotationSum = 0;
                live.sunFrames = live.sunAccepted = live.pairSum = 0;
                if (live.sunSkyFrames > live.sunSkyLogged)
                    Log("URTI direct : lumière du jeu mesurée : soleil / ciel %.2f (rouge %.2f, vert %.2f, bleu %.2f) ; ombre RT face au soleil : "
                        "%.0f %% de la lumière retirée ; %u images mesurées sur 300, %.0f paires par image", live.sunSky[3], live.sunSky[0], live.sunSky[1],
                        live.sunSky[2], 100.0f * live.sunSky[3] / (1.0f + live.sunSky[3]), live.sunSkyFrames - live.sunSkyLogged,
                        double(live.sunSkyPairs) / std::max(1u, live.sunSkyFrames - live.sunSkyLogged));
                else if (live.sunMeasure)
                    Log("URTI direct : lumière du jeu non mesurée (pas d'ombre du jeu à l'écran) : soleil / ciel gardé à %.2f", live.sunSky[3]);
                live.sunSkyLogged = live.sunSkyFrames, live.sunSkyPairs = 0;
                if (const uint64_t n = live.mapCheck[0] + live.mapCheck[1] + live.mapCheck[2] + live.mapCheck[3])
                {
                    Log("URTI direct : carte d'ombres du jeu au point vu (face au soleil) : %.0f %% éclairés, %.0f %% à l'ombre, %.0f %% au bord, "
                        "%.0f %% hors des cascades ; luminance moyenne éclairés %.3f, à l'ombre %.3f (rapport %.2f : bien plus que 1 si la carte lue est la bonne)",
                        100.0 * live.mapCheck[0] / n, 100.0 * live.mapCheck[1] / n, 100.0 * live.mapCheck[2] / n, 100.0 * live.mapCheck[3] / n,
                        live.mapCheck[0] ? live.mapCheck[4] / 1024.0 / live.mapCheck[0] : 0.0, live.mapCheck[1] ? live.mapCheck[5] / 1024.0 / live.mapCheck[1] : 0.0,
                        live.mapCheck[0] && live.mapCheck[1] && live.mapCheck[5] ? (double(live.mapCheck[4]) / live.mapCheck[0]) / (double(live.mapCheck[5]) / live.mapCheck[1]) : 0.0);
                    char depthText[160] = {}, gapText[160] = {};
                    for (int k = 0, a = 0, b = 0; k < 16; k++)
                    {
                        a += std::snprintf(depthText + a, sizeof(depthText) - a, "%s%llu", k ? " " : "", (unsigned long long)live.depthHist[k]);
                        b += std::snprintf(gapText + b, sizeof(gapText) - b, "%s%llu", k ? " " : "", (unsigned long long)live.gapHist[k]);
                    }
                    Log("URTI direct : profondeur des points dans la cascade (-0,5 à 1,5 par 0,125) : %s ; carte - point (-0,02 à 0,02 par 0,0025) : %s",
                        depthText, gapText);
                }
                for (uint64_t& v : live.mapCheck)
                    v = 0;
                for (int k = 0; k < 16; k++)
                    live.depthHist[k] = live.gapHist[k] = 0;
            }
        }
        live.sunReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    if (live.mvConvention == 255 && live.mvErrorsReadback[live.frame % 3])
        if (const uint32_t* e = nullptr; SUCCEEDED(live.mvErrorsReadback[live.frame % 3]->Map(0, nullptr, (void**)&e)) && e)
        {
            const double meanMotion = e[33] ? e[32] / 1e6 / e[33] : 0;
            if (meanMotion > 0.0008) // la caméra bouge : les candidates se départagent
            {
                for (int k = 0; k < 16; k++)
                    live.mvErr[k] += e[k * 2] / 10000.0, live.mvCount[k] += e[k * 2 + 1];
                if (++live.mvMovingFrames >= 90)
                {
                    int best = -1, second = -1;
                    double bestErr = 1e30, secondErr = 1e30;
                    for (int k = 0; k < 16; k++)
                    {
                        if (live.mvCount[k] < 500)
                            continue;
                        const double m = live.mvErr[k] / live.mvCount[k];
                        if (m < bestErr)
                            second = best, secondErr = bestErr, best = k, bestErr = m;
                        else if (m < secondErr)
                            second = k, secondErr = m;
                    }
                    if (best >= 0 && bestErr < 0.7 * secondErr)
                    {
                        live.mvConvention = uint32_t(best);
                        Log("URTI direct : convention des vecteurs de mouvement trouvée (%d : erreur %.4f, suivante %.4f) : accumulation des ombres active", best,
                            bestErr, secondErr);
                        if (live.verified || live.fromStore)
                            LiveSaveCalibration();
                    }
                    else if (++live.mvUndecided >= 3 && live.motionSource == 0 && ++live.mvGiveUps >= 2)
                    {
                        // deux cibles essayées sans vainqueur : les vecteurs du jeu sont illisibles (Horizon Forbidden
                        // West), ceux fabriqués avec le mouvement de la caméra mesuré sur notre géométrie les remplacent
                        live.mvUndecided = 0, live.mvSynthetic = true, live.mvConvention = 0;
                        Log("URTI direct : vecteurs de mouvement du jeu illisibles : mouvement de la caméra mesuré sur notre géométrie à la place");
                        if (live.verified || live.fromStore)
                            LiveSaveCalibration();
                    }
                    else if (live.mvUndecided >= 3)
                    {
                        // trois mesures en mouvement sans vainqueur : ce ne sont pas des vecteurs de mouvement, la cible
                        // suivante du même format est essayée
                        live.mvUndecided = 0, live.mvCandidate++;
                        Log("URTI direct : vecteurs de mouvement jamais départagés (meilleure %.4f, suivante %.4f) : essai de la cible n° %u de ce format",
                            bestErr, secondErr, live.mvCandidate);
                    }
                    else
                        Log("URTI direct : vecteurs de mouvement pas encore départagés (meilleure %.4f, suivante %.4f), on continue", bestErr, secondErr);
                    std::fill(std::begin(live.mvErr), std::end(live.mvErr), 0.0), std::fill(std::begin(live.mvCount), std::end(live.mvCount), 0.0);
                    live.mvMovingFrames = 0;
                }
            }
            live.mvErrorsReadback[live.frame % 3]->Unmap(0, nullptr);
        }
    const effect_texture_variable var = runtime->find_texture_variable("urti.fx", "UrtiDepth");
    resource_view depthSrv = {}, depthSrgb = {};
    if (var.handle)
        runtime->get_texture_binding(var, &depthSrv, &depthSrgb);
    ID3D12Resource* depthCopy = depthSrv.handle ? reinterpret_cast<ID3D12Resource*>(runtime->get_device()->get_resource_from_view(depthSrv).handle) : nullptr;
    const effect_uniform_variable active = runtime->find_uniform_variable("urti.fx", "UrtiActive");
    if (depthCopy && !live.depthReady)
    {
        // la loi de profondeur est mesurée à chaque image sur le GPU : seule la projection est supposée
        LiveSetProjection(depthCopy->GetDesc());
        live.depthReady = true;
    }
    if (slot < 0 || !depthCopy || !live.depthReady)
    {
        if (active.handle)
            runtime->set_uniform_value_bool(active, false);
        zeroCounters(D3D12_RESOURCE_STATE_STREAM_OUT);
        live.frame++;
        return;
    }
    const D3D12_RESOURCE_DESC depthDesc = depthCopy->GetDesc();
    if (live.fromStore && live.p00 == 0)
        LiveSetProjection(depthDesc);
    live.lastFilled = cursorNow / 16;
    if (!LiveEnsureCapacity(device.Get(), std::max<uint64_t>(std::max<uint64_t>(live.lastFilled, 300000), cursorNow / 16)) || !LiveEnsureImages(runtime, g.device, depthDesc) ||
        !LiveEnsureSun(g.device))
    {
        zeroCounters(D3D12_RESOURCE_STATE_STREAM_OUT);
        return;
    }

    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!live.timeHeap)
    {
        D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 24, 0 };
        g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&live.timeHeap));
        for (auto& r : live.timeReadback)
            r = MakeBuffer(g.device, 8 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    // minutage lu trois images plus tard : structure, soleil, ombres, reflets, composition (ms)
    if (const uint64_t* t = nullptr; live.timeReadback[live.frame % 3] && SUCCEEDED(live.timeReadback[live.frame % 3]->Map(0, nullptr, (void**)&t)) && t)
    {
        UINT64 freq = 0;
        if (ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()); q && SUCCEEDED(q->GetTimestampFrequency(&freq)) && freq &&
            t[5] > t[0] && t[5] - t[0] < freq)
        {
            for (int k = 0; k < 5; k++)
                live.timeSum[k] += double(t[k + 1] - t[k]) * 1000.0 / double(freq);
            // rayons d'occultation : un de moins au-dessus de la cible (+15 %), un de plus bien en dessous
            live.shadowMs += (std::min(50.0, double(t[3] - t[2]) * 1000.0 / double(freq)) - live.shadowMs) * 0.2;
            if (live.aoTarget > 0 && live.frame % 30 == 0)
            {
                const uint32_t before = live.aoRays;
                if (live.shadowMs > live.aoTarget * 1.15 && live.aoRays > 2)
                    live.aoRays--;
                else if (live.shadowMs < live.aoTarget * 0.6 && live.aoRays < 4)
                    live.aoRays++;
                if (live.aoRays != before)
                    Log("URTI direct : occultation à %u rayons par pixel calculé (passe des ombres %.2f ms, cible %.2f)", live.aoRays, live.shadowMs,
                        live.aoTarget);
            }
            if (const uint32_t s = uint32_t(live.frame % 3); live.slotBuilt[s])
            {
                const double build = std::min(50.0, double(t[2] - t[1]) * 1000.0 / double(freq)), trace = std::min(100.0, double(t[5] - t[3]) * 1000.0 / double(freq));
                if (!live.slotRefit[s])
                {
                    live.rebuildMs += (build - live.rebuildMs) * 0.2;
                    // budget de géométrie : toutes les 30 images, -15 % au-dessus de la cible (+10 %), +8 % bien en dessous
                    if (live.buildTarget > 0 && live.budgetMax > 0 && live.frame % 30 == 0)
                    {
                        constexpr uint64_t kBudgetMin = 1500000;
                        if (live.rebuildMs > live.buildTarget * 1.1)
                            live.vertexBudget = std::max<uint64_t>(std::min(kBudgetMin, live.budgetMax), live.vertexBudget * 85 / 100);
                        else if (live.rebuildMs < live.buildTarget * 0.75)
                            live.vertexBudget = std::min<uint64_t>(live.budgetMax, live.vertexBudget * 108 / 100);
                    }
                    // le coût de la mise à jour revient lentement vers une valeur plausible : une seule mesure
                    // aberrante (image chargée, autre programme) ne bloque pas les mises à jour pour toujours
                    live.refitMs += (0.3 * live.rebuildMs - live.refitMs) * 0.05;
                    if (live.slotCycle[s] == live.buildCycle)
                        live.traceBase = trace;
                }
                else
                {
                    live.refitMs += (build - live.refitMs) * 0.2;
                    if (live.slotCycle[s] == live.buildCycle && live.traceBase >= 0)
                        live.refitExtra += std::max(0.0, trace - live.traceBase);
                }
                live.slotBuilt[s] = false;
            }
            if (t[7] > t[6] && t[7] - t[6] < freq)
                live.timeSum[5] += double(t[7] - t[6]) * 1000.0 / double(freq);
            if (++live.timeFrames == 300)
            {
                const double n = live.timeFrames;
                Log("URTI direct : GPU par image : structure %.2f ms, soleil et profondeur %.2f, copie carte du jeu %.2f, ombres (%u rayons) %.2f, "
                    "reflets %.2f, composition %.2f", live.timeSum[0] / n, live.timeSum[1] / n, live.timeSum[2] / n, live.shadowRays, live.timeSum[3] / n,
                    live.timeSum[4] / n, live.timeSum[5] / n);
                const uint32_t builds = live.statRebuilds + live.statUpdates;
                if (builds)
                    Log("URTI direct : structure d'accélération : %.0f sommets en moyenne (capacité %llu), %u reconstructions et %u mises à jour "
                        "(reconstruction %.2f ms, mise à jour %.2f ms)", double(live.statVertices) / builds, (unsigned long long)live.capacity,
                        live.statRebuilds, live.statUpdates, live.rebuildMs, live.refitMs);
                if (live.statPlaceFrames)
                {
                    const double f = double(live.statPlaceFrames);
                    Log("URTI direct : places fixes : %.0f tenues (%.0f Mo), partie occupée %.0f Mo sur %.0f, %.1f nouvelles par image, %llu redistributions",
                        live.statPlaces / f, live.statPlaceHeld / f / 1048576.0, live.statPlaceHigh / f / 1048576.0, kSlotBytes / 1048576.0,
                        live.statPlaceNew / f, (unsigned long long)live.statPlaceResets);
                }
                live.statPlaces = live.statPlaceHeld = live.statPlaceHigh = live.statPlaceNew = live.statPlaceFrames = live.statPlaceResets = 0;
                live.statRebuilds = live.statUpdates = 0, live.statVertices = 0;
                if (live.statBudgetFrames)
                {
                    const double f = double(live.statBudgetFrames);
                    Log("URTI direct : budget de géométrie %llu sommets : %.0f dessins dans la scène RT (%.0f sommets réservés), %.0f hors budget, "
                        "%.0f re-mesurés, %.1f refusés (emplacement plein), %.1f entrés ou sortis de la scène RT par image", (unsigned long long)live.vertexBudget,
                        live.statDraws / f, live.statReserved / f, live.statOutOfBudget / f, live.statProbes / f, live.statOverflow / f, live.statChurn / f);
                }
                live.statChurn = 0;
                live.statDraws = live.statOutOfBudget = live.statProbes = live.statOverflow = live.statBudgetFrames = live.statReserved = 0;
                std::fill(std::begin(live.timeSum), std::end(live.timeSum), 0.0);
                live.timeFrames = 0;
            }
        }
        live.timeReadback[live.frame % 3]->Unmap(0, nullptr);
    }
    LiveTime(list.Get(), 0);
    // 1. sommets de l'écran vers l'espace caméra (NaN au-delà de ce qui a été rempli dans chaque zone) ;
    // seulement la partie du flux attribuée à cette image : la structure ne couvre qu'elle
    Transition(list.Get(), g.so.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, srv);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, srv);
    const std::vector<LiveRegion>& tableNow = persistNow ? placesNow : regionsNow; // zones converties (scène RT)
    ID3D12Resource* placesBuf = nullptr;
    if (persistNow)
    {
        ComPtr<ID3D12Resource>& up = live.placeUpload[live.frame % 3];
        const uint64_t bytes = std::max<size_t>(placesNow.size(), 1) * sizeof(LiveRegion);
        if (!up || up->GetDesc().Width < bytes)
            Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        if (up && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            if (!placesNow.empty())
                std::memcpy(p, placesNow.data(), placesNow.size() * sizeof(LiveRegion));
            else
                std::memset(p, 0, sizeof(LiveRegion));
            up->Unmap(0, nullptr);
            placesBuf = up.Get();
        }
        if (!placesBuf)
        {
            zeroCounters(srv), Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
            return;
        }
    }
    ID3D12Resource* regionsBuf = nullptr;
    {
        ComPtr<ID3D12Resource>& up = live.regionUpload[live.frame % 3];
        const uint64_t bytes = std::max<size_t>(regionsNow.size(), 1) * sizeof(LiveRegion);
        if (!up || up->GetDesc().Width < bytes)
            Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        if (up && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            if (!regionsNow.empty())
                std::memcpy(p, regionsNow.data(), regionsNow.size() * sizeof(LiveRegion));
            else
                std::memset(p, 0, sizeof(LiveRegion));
            up->Unmap(0, nullptr);
            regionsBuf = up.Get();
        }
    }
    if (!regionsBuf)
    {
        zeroCounters(srv), Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
        return;
    }
    // par zone : identité, début de la zone du même objet dans l'image précédente (+ 1 ; 0 : inconnue)
    ID3D12Resource* linksBuf = nullptr;
    {
        live.pendingStart.clear();
        std::vector<uint32_t> links(std::max<size_t>(tableNow.size(), 1) * 2, 0);
        for (size_t i = 0; i < tableNow.size(); i++)
        {
            const LiveRegion& r = tableNow[i];
            links[i * 2] = r.id;
            if (!r.id)
                continue;
            if (live.prevValid)
                if (auto it = live.prevStart.find(r.id); it != live.prevStart.end())
                    links[i * 2 + 1] = it->second + 1;
            live.pendingStart.emplace(r.id, r.offset / 16);
        }
        ComPtr<ID3D12Resource>& up = live.linkUpload[live.frame % 3];
        const uint64_t bytes = links.size() * 4;
        if (!up || up->GetDesc().Width < bytes)
            Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        if (up && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            std::memcpy(p, links.data(), bytes);
            up->Unmap(0, nullptr);
            linksBuf = up.Get();
        }
    }
    // début dans le flux de la zone de chaque entrée de la table convertie ; zones de cette image triées par leur
    // début dans le flux, pour la loi de profondeur (live_depthfit y tire ses sommets)
    const std::vector<uint32_t>& srcTable = persistNow ? placeSrc : srcNow;
    std::vector<LiveRegion> fitNow(regionsNow.size());
    uint32_t fitEnd = 0;
    for (size_t i = 0; i < regionsNow.size(); i++)
        fitNow[i] = { srcNow[i], regionsNow[i].capacity, regionsNow[i].counter, regionsNow[i].id }, fitEnd = std::max(fitEnd, srcNow[i] + regionsNow[i].capacity);
    std::sort(fitNow.begin(), fitNow.end(), [](const LiveRegion& a, const LiveRegion& b) { return a.offset < b.offset; });
    auto upload = [&](ComPtr<ID3D12Resource>& up, const void* data, uint64_t bytes) -> ID3D12Resource* {
        bytes = std::max<uint64_t>(bytes, 16);
        if (!up || up->GetDesc().Width < bytes)
            Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        if (!up || FAILED(up->Map(0, nullptr, &p)))
            return nullptr;
        std::memset(p, 0, 16);
        if (data)
            std::memcpy(p, data, bytes);
        up->Unmap(0, nullptr);
        return up.Get();
    };
    ID3D12Resource* srcBuf = upload(live.srcUpload[live.frame % 3], srcTable.empty() ? nullptr : srcTable.data(), srcTable.size() * 4);
    ID3D12Resource* fitBuf = upload(live.fitUpload[live.frame % 3], fitNow.empty() ? nullptr : fitNow.data(), fitNow.size() * sizeof(LiveRegion));
    if (!linksBuf || !srcBuf || !fitBuf)
    {
        zeroCounters(srv), Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
        return;
    }
    // table des zones de la scène RT copiée en mémoire vidéo : chaque sommet (conversion), chaque reflet et le cache
    // de surfaces y cherchent leur zone ; dans le tampon d'envoi, chaque lecture traversait le bus PCIe
    ID3D12Resource* sceneTable = persistNow ? placesBuf : regionsBuf;
    {
        const uint64_t bytes = std::max<size_t>(tableNow.size(), 1) * sizeof(LiveRegion);
        if (!live.sceneGpu || live.sceneGpu->GetDesc().Width < bytes)
        {
            Retire(live.sceneGpu);
            live.sceneGpu = UavBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_RESOURCE_STATE_COPY_DEST);
            live.sceneGpuReadable = false;
        }
        if (live.sceneGpu)
        {
            if (live.sceneGpuReadable)
                Transition(list.Get(), live.sceneGpu.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.sceneGpu.Get(), 0, sceneTable, 0, bytes);
            Transition(list.Get(), live.sceneGpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            live.sceneGpuReadable = true;
            sceneTable = live.sceneGpu.Get();
        }
    }
    list->SetComputeRootSignature(live.convertRs.Get());
    list->SetPipelineState(live.convertPso.Get());
    list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
    list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, sceneTable->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, srcBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, live.view->GetGPUVirtualAddress());
    if (!live.pairsBuf)
    {
        live.pairsBuf = UavBuffer(g.device, 16 + uint64_t(kLiveMaxMotionPairs) * 24, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.motionBuf = UavBuffer(g.device, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto& r : live.motionReadback)
            r = MakeBuffer(g.device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    list->SetComputeRootUnorderedAccessView(5, live.pairsBuf ? live.pairsBuf->GetGPUVirtualAddress() : live.view->GetGPUVirtualAddress());
    const uint32_t converted = std::max(3u, uint32_t(std::min<uint64_t>(cursorNow / 16, live.capacity)) / 3 * 3);
    const struct { float p00, p11; uint32_t count, regionCount, maxPairs, pad0, pad1, pad2; } cc = {
        live.p00, live.p11, converted, uint32_t(tableNow.size()), live.pairsBuf ? kLiveMaxMotionPairs : 0u, 0, 0, 0 };
    list->SetComputeRoot32BitConstants(6, 8, &cc, 0);
    list->Dispatch(UINT((converted + 255) / 256), 1, 1);
    // 1d. mouvement de la caméra depuis l'image précédente, mesuré sur les paires de sommets de la conversion
    if (live.camfitPso && live.pairsBuf && live.motionBuf && live.motionReadback[live.frame % 3])
    {
        D3D12_RESOURCE_BARRIER pb = {};
        pb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        pb.UAV.pResource = live.pairsBuf.Get();
        list->ResourceBarrier(1, &pb);
        list->SetComputeRootSignature(live.camfitRs.Get());
        list->SetPipelineState(live.camfitPso.Get());
        list->SetComputeRootUnorderedAccessView(0, live.motionBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, live.pairsBuf->GetGPUVirtualAddress());
        const uint32_t fc[4] = { kLiveMaxMotionPairs, 0, 0, 0 };
        list->SetComputeRoot32BitConstants(2, 4, fc, 0);
        list->Dispatch(1, 1, 1);
        Transition(list.Get(), live.motionBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.motionReadback[live.frame % 3].Get(), 0, live.motionBuf.Get(), 0, 128);
        Transition(list.Get(), live.motionBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // 1e. surfaces transparentes (eau, vitres) : leur couche convertie, puis seules les surfaces planes gardées
    live.transpBuilt = false;
    if (live.waterOn && !transpNow.empty() && live.planesPso)
    {
        if (!live.viewT)
        {
            live.viewT = UavBuffer(g.device, uint64_t(kLiveTranspVertices) * 12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.planeStats = UavBuffer(g.device, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (auto& r : live.planeReadback)
                r = MakeBuffer(g.device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RAYTRACING_GEOMETRY_DESC tg = {};
            tg.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            tg.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            tg.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            tg.Triangles.VertexBuffer.StrideInBytes = 12;
            tg.Triangles.VertexCount = kLiveTranspVertices;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti = {};
            ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            ti.NumDescs = 1;
            ti.pGeometryDescs = &tg;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
            device->GetRaytracingAccelerationStructurePrebuildInfo(&ti, &pi);
            live.blasT = UavBuffer(g.device, pi.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            live.blasTScratch = UavBuffer(g.device, pi.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.viewTRead = false;
            Log("URTI direct : couche des surfaces transparentes pour %u sommets (%.1f Mo)", kLiveTranspVertices, pi.ResultDataMaxSizeInBytes / 1048576.0);
        }
        auto uploadT = [&](ComPtr<ID3D12Resource>& up, const void* data, uint64_t bytes) -> ID3D12Resource* {
            if (!up || up->GetDesc().Width < bytes)
                Retire(up), up = MakeBuffer(g.device, std::max<uint64_t>(bytes * 2, 65536), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p = nullptr;
            if (!up || FAILED(up->Map(0, nullptr, &p)))
                return nullptr;
            std::memcpy(p, data, bytes);
            up->Unmap(0, nullptr);
            return up.Get();
        };
        ID3D12Resource* tTable = uploadT(live.transpUpload[live.frame % 3], transpNow.data(), transpNow.size() * sizeof(LiveRegion));
        ID3D12Resource* tSrc = uploadT(live.transpSrcUpload[live.frame % 3], transpSrcNow.data(), transpSrcNow.size() * 4);
        const uint32_t tCount = std::max(3u, std::min(transpCursorNow / 16, kLiveTranspVertices) / 3 * 3);
        if (live.viewT && live.blasT && live.blasTScratch && tTable && tSrc && live.planeStats && live.planeReadback[live.frame % 3])
        {
            if (live.viewTRead)
                Transition(list.Get(), live.viewT.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), live.viewTRead = false;
            list->SetComputeRootSignature(live.convertRs.Get());
            list->SetPipelineState(live.convertPso.Get());
            list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress()); // débuts dans tout le flux
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, tTable->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, tSrc->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, live.viewT->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(5, live.pairsBuf ? live.pairsBuf->GetGPUVirtualAddress() : live.viewT->GetGPUVirtualAddress());
            const struct { float p00, p11; uint32_t count, regionCount, maxPairs, pad0, pad1, pad2; } tc = {
                live.p00, live.p11, tCount, uint32_t(transpNow.size()), 0, 0, 0, 0 };
            list->SetComputeRoot32BitConstants(6, 8, &tc, 0);
            list->Dispatch(UINT((tCount + 255) / 256), 1, 1);
            D3D12_RESOURCE_BARRIER vb = {};
            vb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            vb.UAV.pResource = live.viewT.Get();
            list->ResourceBarrier(1, &vb);
            // tri : surfaces planes gardées, le reste invisible aux rayons ; bilan lu trois images plus tard
            Transition(list.Get(), live.planeStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.planeStats.Get(), 0, g.zeros.Get(), 0, 16);
            Transition(list.Get(), live.planeStats.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetComputeRootSignature(live.planesRs.Get());
            list->SetPipelineState(live.planesPso.Get());
            list->SetComputeRootShaderResourceView(0, tTable->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2, live.viewT->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.planeStats->GetGPUVirtualAddress());
            const uint32_t pc[4] = { uint32_t(transpNow.size()), 0, 0, 0 };
            list->SetComputeRoot32BitConstants(4, 4, pc, 0);
            list->Dispatch(UINT(transpNow.size()), 1, 1);
            Transition(list.Get(), live.planeStats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.planeReadback[live.frame % 3].Get(), 0, live.planeStats.Get(), 0, 16);
            Transition(list.Get(), live.planeStats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list.Get(), live.viewT.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv), live.viewTRead = true;
            // sa structure, reconstruite à chaque image (quelques surfaces)
            D3D12_RAYTRACING_GEOMETRY_DESC tg = {};
            tg.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            tg.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            tg.Triangles.VertexBuffer = { live.viewT->GetGPUVirtualAddress(), 12 };
            tg.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            tg.Triangles.VertexCount = tCount;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tb = {};
            tb.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            tb.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            tb.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            tb.Inputs.NumDescs = 1;
            tb.Inputs.pGeometryDescs = &tg;
            tb.DestAccelerationStructureData = live.blasT->GetGPUVirtualAddress();
            tb.ScratchAccelerationStructureData = live.blasTScratch->GetGPUVirtualAddress();
            list->BuildRaytracingAccelerationStructure(&tb, 0, nullptr);
            vb.UAV.pResource = live.blasT.Get();
            list->ResourceBarrier(1, &vb);
            live.transpBuilt = true;
        }
    }
    // bilan des surfaces transparentes, lu trois images plus tard
    if (const uint32_t* c = nullptr; live.planeReadback[live.frame % 3] && SUCCEEDED(live.planeReadback[live.frame % 3]->Map(0, nullptr, (void**)&c)) && c)
    {
        live.planeFrames++, live.planeKept += c[0], live.planeTris += c[1], live.planeSeen += c[2];
        live.planeReadback[live.frame % 3]->Unmap(0, nullptr);
        if (live.planeFrames >= 300)
        {
            Log("URTI direct : surfaces transparentes : %.1f dessins par image, dont %.1f planes gardées (eau, vitres : %.0f triangles)",
                double(live.planeSeen) / live.planeFrames, double(live.planeKept) / live.planeFrames, double(live.planeTris) / live.planeFrames);
            live.planeFrames = live.planeKept = live.planeTris = live.planeSeen = 0;
        }
    }
    // 1a. mesure de chaque objet capté, de la scène RT et de la zone de mesure (budget de géométrie)
    if (live.statsPso && !(regionsNow.empty() && probesNow.empty()))
    {
        const size_t n = regionsNow.size() + probesNow.size();
        const uint64_t bytes = uint64_t(n) * 16;
        if (live.statsBytes < bytes)
        {
            const uint64_t size = std::max<uint64_t>(bytes * 2, 65536);
            Retire(live.statsBuf);
            live.statsBuf = UavBuffer(g.device, size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (int k = 0; k < 3; k++)
            {
                Retire(live.statsUpload[k]), Retire(live.statsReadback[k]);
                live.statsUpload[k] = MakeBuffer(g.device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                live.statsReadback[k] = MakeBuffer(g.device, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                live.statsIds[k].clear();
            }
            live.statsBytes = size;
        }
        ID3D12Resource* up = live.statsUpload[live.frame % 3].Get();
        void* p = nullptr;
        if (live.statsBuf && up && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            for (size_t i = 0; i < regionsNow.size(); i++)
            {
                const LiveRegion r = { srcNow[i], regionsNow[i].capacity, regionsNow[i].counter, regionsNow[i].id };
                std::memcpy(static_cast<char*>(p) + i * sizeof(LiveRegion), &r, sizeof(r));
            }
            std::memcpy(static_cast<char*>(p) + regionsNow.size() * sizeof(LiveRegion), probesNow.data(), probesNow.size() * sizeof(LiveRegion));
            up->Unmap(0, nullptr);
            list->SetComputeRootSignature(live.statsRs.Get());
            list->SetPipelineState(live.statsPso.Get());
            list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, up->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.statsBuf->GetGPUVirtualAddress());
            const struct { float p00, p11; uint32_t count, pad; } sc = { live.p00, live.p11, uint32_t(n), 0 };
            list->SetComputeRoot32BitConstants(4, 4, &sc, 0);
            list->Dispatch(UINT(n), 1, 1);
            Transition(list.Get(), live.statsBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.statsReadback[live.frame % 3].Get(), 0, live.statsBuf.Get(), 0, bytes);
            Transition(list.Get(), live.statsBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            std::vector<uint32_t>& ids = live.statsIds[live.frame % 3];
            std::vector<uint32_t>& caps = live.statsCaps[live.frame % 3];
            ids.clear(), caps.clear();
            for (const LiveRegion& r : regionsNow)
                ids.push_back(r.id), caps.push_back(r.capacity / 16);
            for (const LiveRegion& r : probesNow)
                ids.push_back(r.id), caps.push_back(r.capacity / 16);
        }
        live.statDraws += regionsNow.size(), live.statProbes += probesNow.size(), live.statBudgetFrames++;
    }
    Transition(list.Get(), live.view.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);

    LiveTime(list.Get(), 1);
    // 1b. soleil de cette image : paires écran / carte d'ombres de même signature, ajustées puis votées
    if (live.fovReset)
    {
        // projection changée : la mesure de l'étirement repart de zéro
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.sunBuf.Get(), 1200, g.zeros.Get(), 0, 24);
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.fovReset = false;
    }
    if (live.sunDirty)
    {
        void* p = nullptr;
        if (SUCCEEDED(live.sunUpload->Map(0, nullptr, &p)))
        {
            const float s[12] = { live.sun[0], live.sun[1], live.sun[2], 1, 0, 0, 0, 0, 0, live.A, live.B, std::isfinite(live.A) && std::isfinite(live.B) && live.B != 0 ? 1.0f : 0.0f };
            std::memcpy(p, s, sizeof(s));
            live.sunUpload->Unmap(0, nullptr);
        }
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.sunBuf.Get(), 0, live.sunUpload.Get(), 0, 48);
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.sunDirty = false;
    }
    struct Pair
    {
        uint32_t camBase, mapBase, camCounter, mapCounter; // débuts dans le flux (octets), compteurs
        float viewport[4];
        float weight;
        uint32_t camCapacity, mapCapacity, pad;
    };
    std::vector<Pair> pairs;
    {
        std::unordered_map<uint64_t, std::vector<uint32_t>> sunByKey;
        for (uint32_t i = 0; i < marksNow.size(); i++)
            if (marksNow[i].sunMap)
                sunByKey[marksNow[i].key].push_back(i);
        std::unordered_map<uint64_t, uint32_t> rank;
        for (uint32_t i = 0; i < marksNow.size() && pairs.size() < kLiveMaxPairs; i++)
        {
            if (marksNow[i].sunMap)
                continue;
            auto it = sunByKey.find(marksNow[i].key);
            if (it == sunByKey.end())
                continue;
            const uint32_t r = rank[marksNow[i].key]++;
            const std::vector<uint32_t>& c = it->second;
            // même rang dans les deux passes : très probablement le même objet (poids 8)
            auto add = [&](uint32_t m, float w) {
                const LiveMark &a = marksNow[i], &b = marksNow[m];
                pairs.push_back({ uint32_t(a.slot * kSlotBytes + a.offset), uint32_t(b.slot * kSlotBytes + b.offset), a.counter, b.counter,
                                  { b.viewport[0], b.viewport[1], b.viewport[2], b.viewport[3] }, w, a.capacity, b.capacity, 0 });
            };
            if (r < c.size())
                add(c[r], 8.0f);
            for (uint32_t k = 0; k < c.size() && k < 3 && pairs.size() < kLiveMaxPairs; k++)
                if (k != r)
                    add(c[k], 1.0f);
        }
    }
    {
        void* p = nullptr;
        ID3D12Resource* up = live.pairsUpload[live.frame % 3].Get();
        if (!pairs.empty() && SUCCEEDED(up->Map(0, nullptr, &p)))
        {
            std::memcpy(p, pairs.data(), pairs.size() * sizeof(Pair));
            up->Unmap(0, nullptr);
            list->SetComputeRootSignature(live.fitRs.Get());
            list->SetPipelineState(live.fitPso.Get());
            list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, up->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.pairDirs->GetGPUVirtualAddress());
            const struct { float p00, p11; uint32_t count, slotBytes; } fc = { live.p00, live.p11, uint32_t(pairs.size()), uint32_t(kSlotBytes) };
            list->SetComputeRoot32BitConstants(4, 4, &fc, 0);
            list->Dispatch(UINT(pairs.size()), 1, 1);
            Transition(list.Get(), live.pairDirs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.voteRs.Get());
            list->SetPipelineState(live.votePso.Get());
            list->SetComputeRootShaderResourceView(0, live.pairDirs->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(1, live.sunBuf->GetGPUVirtualAddress());
            const struct { uint32_t count; float cosTol, minShare, sunSign, jitterDelta, jitterAbsolute, pad0, pad1; } vc = {
                uint32_t(pairs.size()), std::cos(2.0f * 3.14159265f / 180.0f), 0.35f, live.sunSign, live.jitterMode >= 1 ? 1.0f : 0.0f,
                live.jitterMode >= 2 ? 1.0f : 0.0f, 0, 0 };
            list->SetComputeRoot32BitConstants(2, 8, &vc, 0);
            list->Dispatch(1, 1, 1);
            Transition(list.Get(), live.pairDirs.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            // aucune paire : bilan « non mesuré », le soleil précédent reste, plus de cascade connue
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.sunBuf.Get(), 16, g.zeros.Get(), 0, 20); // pas la loi de profondeur (36 à 47)
            list->CopyBufferRegion(live.sunBuf.Get(), 624, g.zeros.Get(), 0, 32); // rotation non mesurée, plus de cascades gardées
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    // 1c. loi de profondeur de cette image, mesurée sur les sommets de l'écran
    {
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = live.sunBuf.Get();
        list->ResourceBarrier(1, &ub);
        list->SetComputeRootSignature(live.depthfitRs.Get());
        list->SetPipelineState(live.depthfitPso.Get());
        list->SetComputeRootShaderResourceView(0, g.so->GetGPUVirtualAddress() + slot * kSlotBytes);
        list->SetComputeRootShaderResourceView(1, g.counters->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, fitBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3, live.sunBuf->GetGPUVirtualAddress());
        const uint32_t dc[4] = { uint32_t(fitNow.size()), fitEnd, 0, 0 };
        list->SetComputeRoot32BitConstants(4, 4, dc, 0);
        list->Dispatch(1, 1, 1);
    }
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);

    // 2. structures d'accélération
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer = { live.view->GetGPUVirtualAddress(), 12 };
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = converted;
    // même liste de dessins (zones, tailles, identités) qu'à la dernière construction : les triangles sont
    // les mêmes, seules leurs positions ont changé (caméra, vent, objets mobiles) : mise à jour
    uint64_t layout = 1469598103934665603ull ^ converted;
    for (const LiveRegion& r : regionsNow)
        layout = (layout ^ (uint64_t(r.offset) << 32 ^ r.capacity ^ uint64_t(r.id) << 17)) * 1099511628211ull;
    const bool refit = live.blasRefit > 0 && live.blasAge < live.blasRefit &&
                       (persistNow ? converted == live.blasConverted && !live.placesMoved && live.refitExtra < live.rebuildMs - live.refitMs
                                   : layout == live.blasLayout);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                      (refit ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(0));
    if (refit)
        bd.SourceAccelerationStructureData = live.blas->GetGPUVirtualAddress(), live.blasAge++, live.statUpdates++;
    else
        live.blasLayout = layout, live.blasConverted = converted, live.blasAge = 0, live.statRebuilds++,
        live.buildCycle++, live.refitExtra = 0.0, live.traceBase = -1.0;
    live.slotCycle[live.frame % 3] = live.buildCycle, live.slotRefit[live.frame % 3] = refit, live.slotBuilt[live.frame % 3] = true;
    live.statVertices += converted;
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.NumDescs = 1;
    bd.Inputs.pGeometryDescs = &geom;
    bd.DestAccelerationStructureData = live.blas->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = live.blasScratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = live.blas.Get();
    list->ResourceBarrier(1, &uav);
    {
        // exemplaire 0 : la scène opaque (masque 1 : occultation, reflets, cache) ; exemplaire 1 : les surfaces
        // transparentes planes (masque 2 : seul le rayon de la caméra des reflets les cherche ; masque 0 sans elles)
        D3D12_RAYTRACING_INSTANCE_DESC inst[2] = {};
        for (auto& i : inst)
            i.Transform[0][0] = i.Transform[1][1] = i.Transform[2][2] = 1.0f;
        inst[0].InstanceMask = 0x01;
        inst[0].AccelerationStructure = live.blas->GetGPUVirtualAddress();
        inst[1].InstanceID = 1;
        inst[1].InstanceMask = live.transpBuilt ? 0x02 : 0x00;
        inst[1].AccelerationStructure = live.transpBuilt ? live.blasT->GetGPUVirtualAddress() : live.blas->GetGPUVirtualAddress();
        void* p = nullptr;
        if (SUCCEEDED(live.instances->Map(0, nullptr, &p)))
        {
            std::memcpy(p, inst, sizeof(inst));
            live.instances->Unmap(0, nullptr);
        }
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    td.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    td.Inputs.NumDescs = 2;
    td.Inputs.InstanceDescs = live.instances->GetGPUVirtualAddress();
    td.DestAccelerationStructureData = live.tlas->GetGPUVirtualAddress();
    td.ScratchAccelerationStructureData = live.tlasScratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    uav.UAV.pResource = live.tlas.Get();
    list->ResourceBarrier(1, &uav);

    // 3a. profondeur de l'image précédente gardée (accumulation dans le temps)
    if (live.depthPrevBuf)
    {
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(live.depthPrevBuf.Get(), live.depthBuf.Get());
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    // 3. profondeur du jeu (copie de ReShade) vers un tampon lisible par le shader
    const D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(list.Get(), depthCopy, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT64 total = 0;
        g.device->GetCopyableFootprints(&depthDesc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = live.depthBuf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = depthCopy;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(list.Get(), depthCopy, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);
    Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    // 3c. vecteurs de mouvement fabriqués avec le mouvement de la caméra mesuré (ceux du jeu sont illisibles)
    if (live.mvSynthetic && live.mvsynthPso && live.motionBuf)
    {
        const uint32_t pitch = (live.width * 4 + 255) & ~255u;
        const uint64_t bytes = uint64_t(pitch) * live.height;
        if (!live.mvBuf || live.mvBytes < bytes)
        {
            Retire(live.mvBuf);
            live.mvBuf = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
            live.mvBytes = live.mvBuf ? bytes : 0;
            Log("URTI direct : vecteurs de mouvement fabriqués %ux%u (mouvement de la caméra mesuré sur notre géométrie)", live.width, live.height);
        }
        if (live.mvBuf)
        {
            const bool d24m = depthDesc.Format == DXGI_FORMAT_R24G8_TYPELESS || depthDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
            Transition(list.Get(), live.mvBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list.Get(), live.motionBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.mvsynthRs.Get());
            list->SetPipelineState(live.mvsynthPso.Get());
            list->SetComputeRootShaderResourceView(0, live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, live.motionBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, live.mvBuf->GetGPUVirtualAddress());
            const struct { uint32_t width, height, depthPitch, depth24, mvPitch; float p00, p11; uint32_t pad; } mc = {
                live.width, live.height, live.depthPitch, d24m ? 1u : 0u, pitch, live.p00, live.p11, 0 };
            list->SetComputeRoot32BitConstants(4, 8, &mc, 0);
            list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
            Transition(list.Get(), live.motionBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list.Get(), live.mvBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            live.mvPitch = pitch, live.mvWidth = live.width, live.mvHeight = live.height, live.mvFrame = live.frame, live.mvConvention = 0;
        }
    }

    LiveTime(list.Get(), 2);
    // 3b. carte d'ombres du jeu (son ombre est recalculée pixel par pixel : nos ombres ne s'ajoutent
    // que là où le jeu n'en met pas)
    bool atlasValid = false;
    // (lue aussi pour reconnaître les trous du feuillage découpé : la carte est la vue du soleil)
    if (ID3D12Resource* atlas = reinterpret_cast<ID3D12Resource*>(g_liveSunDepth); atlas && (g_liveCombine || live.alphaScreen) && live.calibrated)
    {
        uint32_t state = 0;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g.depthStates.find(g_liveSunDepth);
            if (it != g.depthStates.end())
                state = PassD3D12State(it->second), known = true;
        }
        const D3D12_RESOURCE_DESC ad = atlas->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT afp = {};
        UINT64 atotal = 0;
        g.device->GetCopyableFootprints(&ad, 0, 1, 0, &afp, nullptr, nullptr, &atotal);
        if (known && (!live.atlasBuf || live.atlasBytes < atotal))
        {
            Retire(live.atlasBuf);
            live.atlasBuf = UavBuffer(g.device, atotal, D3D12_RESOURCE_STATE_COPY_DEST);
            live.atlasBytes = atotal;
            Log("URTI direct : carte d'ombres du jeu %llux%u format %d recopiée à chaque image (%.0f Mo)", (unsigned long long)ad.Width, ad.Height,
                int(ad.Format), atotal / 1048576.0);
        }
        if (known && live.atlasBuf)
        {
            live.atlasPitch = afp.Footprint.RowPitch;
            live.atlasD24 = ad.Format == DXGI_FORMAT_R24G8_TYPELESS || ad.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
            live.atlas16 = ad.Format == DXGI_FORMAT_R16_TYPELESS || ad.Format == DXGI_FORMAT_D16_UNORM || ad.Format == DXGI_FORMAT_R16_UNORM;
            Transition(list.Get(), atlas, D3D12_RESOURCE_STATES(state), D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = live.atlasBuf.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = afp;
            src.pResource = atlas;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list.Get(), atlas, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATES(state));
            Transition(list.Get(), live.atlasBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            atlasValid = true;
            live.atlasFrame = live.frame;
        }
    }

    LiveTime(list.Get(), 3);
    // masque de l'image précédente gardé (images générées, mode mélange)
    if (live.prevShadowBuf)
    {
        Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.prevShadowBuf.Get(), 0, live.shadowBuf.Get(), 0, uint64_t(live.shadowPitch) * live.height);
        Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // rebond de lumière : rayons d'occultation au quart, cache de surfaces rempli et couvrant toute la scène, table
    // des zones en mémoire vidéo
    const bool giNow = live.giStrength > 0 && live.aoAmount > 0 && live.aoQuarter && !live.sunShadows && live.shadowRawBuf && live.denoisePso &&
                       live.giRaw && live.giBuf && live.giPrev && live.surfCacheOn && live.surfCache && live.surfFilled > 0 &&
                       live.surfTriangles >= live.capacity / 3 && live.sceneGpuReadable && sceneTable == live.sceneGpu.Get();
    if (giNow && live.giValid)
    {
        Transition(list.Get(), live.giBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.giPrev.Get(), 0, live.giBuf.Get(), 0, 3ull * live.shadowPitch * live.height);
        Transition(list.Get(), live.giBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (giNow)
        Transition(list.Get(), live.surfCache.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    // 4. un rayon par pixel vers le soleil
    list->SetComputeRootSignature(live.shadowRs.Get());
    list->SetPipelineState(live.shadowPso.Get());
    list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, live.sunBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, atlasValid ? live.atlasBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    const bool motionOk = live.mvBuf && live.mvFrame != ~0ull && live.frame <= live.mvFrame + 1 && live.mvWidth >= live.width &&
                          live.mvHeight >= live.height && live.depthPrevBuf;
    if (motionOk)
    {
        Transition(list.Get(), live.mvBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
        Transition(list.Get(), live.depthPrevBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    }
    if (live.prevShadowBuf)
        Transition(list.Get(), live.prevShadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(4, live.prevShadowBuf ? live.prevShadowBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(5, motionOk ? live.depthPrevBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(6, motionOk ? live.mvBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(7, giNow ? live.surfCache->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(8, giNow ? sceneTable->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(9, live.shadowRawBuf ? live.shadowRawBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(11, giNow ? live.giRaw->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    const bool d24 = depthDesc.Format == DXGI_FORMAT_R24G8_TYPELESS || depthDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    // SIGMA (NVIDIA) : un rayon par pixel, pénombre et poids des corrections ; débruitage ensuite
#if URTI_NRD
    const bool nrdActive = live.nrdShadows && motionOk && live.mvConvention < 16 && live.shadowRawBuf && NrdEnsure(runtime);
    list->SetComputeRootUnorderedAccessView(10, nrdActive ? g_nrd.penumbraBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
#else
    const bool nrdActive = false;
    list->SetComputeRootUnorderedAccessView(10, live.shadowBuf->GetGPUVirtualAddress());
#endif
    // soleil mesuré dans les 30 dernières images (3 de retard de lecture) : sinon pas d'ombres RT
    // ciel couvert, crépuscule : la part du soleil mesurée dans l'image est si faible qu'une ombre RT ne se
    // verrait pas (moins de 13 % de la lumière retirée) : pas de rayons d'ombre (plusieurs ms gagnées)
    const bool sunWeak = live.sunMeasure && live.sunSkyFrames && live.sunSky[3] < 0.15f;
    if (sunWeak != live.sunWeakLogged)
        Log("URTI direct : ombres RT %s (soleil / ciel mesuré %.2f)", sunWeak ? "suspendues : lumière directe trop faible pour se voir" : "reprises", live.sunSky[3]);
    live.sunWeakLogged = sunWeak;
    const bool sunNow = live.calibrated && live.framesSinceSun < 30 && !sunWeak;
    // (sans soleil, la passe garde l'occultation ambiante : flags 16 ne coupe que les rayons du soleil)
    const uint32_t flags = (atlasValid ? 1u : 0u) | (live.atlasD24 ? 2u : 0u) | (live.atlas16 ? 32u : 0u) | (g_liveCombine ? 4u : 0u) | (g_liveDebugGame ? 8u : 0u) |
                           (sunNow ? 0u : 16u) | (live.sunShadows ? 0u : 64u) | (live.shadowRays << 8) | (live.alphaScreen ? 2048u : 0u) |
                           (nrdActive ? 4096u : 0u) |
                           (live.aoQuarter && !live.sunShadows && !nrdActive && live.shadowRawBuf && live.denoisePso ? 128u | (live.aoRays << 16) : 0u) |
                           (giNow && !nrdActive ? 1u << 20 : 0u) | (live.aoRough ? 1u << 21 : 0u);
    const struct
    {
        float sunSign, gameBias, unused, bias;
        uint32_t width, height, depthPitch, shadowPitch;
        float A, B, p00, p11, clearDepth;
        uint32_t d24, atlasPitch, flags;
        uint32_t motionPitch, motionConvention;
        float temporalAlpha;
        uint32_t debugMode, frameIndex;
        float sunSky;
        uint32_t gameDilate, gameMin;
        float aoAmount, aoRadius;
        uint32_t aoMotion, cacheTriangles, regionCount;
    } sc = { live.sunSign, g_liveGameBias, g_liveSunRadius * 3.14159265f / 180.0f, g_liveRayBias, live.width, live.height, live.depthPitch, live.shadowPitch,
             live.A, live.B, live.p00, live.p11, g.depthReversed ? 0.0f : 1.0f, d24 ? 1u : 0u, live.atlasPitch, flags,
             live.mvPitch, live.shadowRawBuf ? 255u : (motionOk && live.prevShadowBuf ? live.mvConvention : 255u), live.temporalAlpha,
             0u, uint32_t(live.frame), live.sunMeasure ? live.sunSky[3] : 0.0f, live.gameDilate, live.gameMin ? 1u : 0u,
             live.aoAmount, live.aoRadius, motionOk && live.mvConvention < 16 ? live.mvConvention : 255u,
             giNow ? uint32_t(live.surfTriangles) : 0u, giNow ? uint32_t(tableNow.size()) : 0u };
    list->SetComputeRoot32BitConstants(12, 29, &sc, 0);
    list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
    if (giNow)
        Transition(list.Get(), live.surfCache.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // débruitage : SIGMA de NVIDIA ; sinon celui d'URTI (flou guidé par la profondeur, historique reprojeté,
    // bornage statistique, mélange adaptatif)
    bool nrdDone = false;
#if URTI_NRD
    if (nrdActive)
    {
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = nullptr; // les deux sorties du tracé d'ombres
        list->ResourceBarrier(1, &ub);
        nrdDone = NrdShadows(runtime, list.Get(), d24, motionOk);
        live.statNrd += nrdDone ? 1 : 0;
    }
#endif
    if (!nrdDone && live.shadowRawBuf && live.denoisePso)
    {
        Transition(list.Get(), live.shadowRawBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        list->SetComputeRootSignature(live.denoiseRs.Get());
        list->SetPipelineState(live.denoisePso.Get());
        list->SetComputeRootShaderResourceView(0, live.shadowRawBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, live.prevShadowBuf ? live.prevShadowBuf->GetGPUVirtualAddress() : live.shadowRawBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, live.depthBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(3, motionOk ? live.depthPrevBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(4, motionOk ? live.mvBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(5, live.sunBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(6, live.shadowBuf->GetGPUVirtualAddress());
        const struct
        {
            uint32_t width, height, depthPitch, shadowPitch, motionPitch, motionConvention, depth24, debugMode;
            float alphaMin, alphaMax, gamma;
            uint32_t blurRadius;
            float p00, p11;
        } dn = { live.width, live.height, live.depthPitch, live.shadowPitch, live.mvPitch,
                 motionOk && live.prevShadowBuf ? live.mvConvention : 255u, d24 ? 1u : 0u, live.debugView >= 1 && live.debugView <= 3 ? live.debugView : 0u,
                 // occultation ambiante seule (ombres du soleil coupées) : elle varie lentement, plus d'images
                 // accumulées et un bornage plus large (moins de grain qui tremble)
                 live.sunShadows ? 0.1f : 0.06f, live.sunShadows ? 0.5f : 0.3f, live.sunShadows ? 1.5f : 2.0f, live.sunShadows ? 1u : 2u,
                 live.p00, live.p11 };
        list->SetComputeRoot32BitConstants(7, 14, &dn, 0);
        list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
        Transition(list.Get(), live.shadowRawBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (giNow && !nrdActive)
        {
            Transition(list.Get(), live.giRaw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            if (live.giValid)
                Transition(list.Get(), live.giPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            list->SetComputeRootShaderResourceView(0, live.giRaw->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.giValid ? live.giPrev->GetGPUVirtualAddress() : live.giRaw->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(6, live.giBuf->GetGPUVirtualAddress());
            auto gn = dn;
            gn.motionConvention = motionOk && live.giValid ? live.mvConvention : 255u;
            gn.debugMode = 0;
            gn.blurRadius = 2u | 256u | 512u; // 5×5, jamais calculé -> 0, trois plans
            list->SetComputeRoot32BitConstants(7, 14, &gn, 0);
            list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
            Transition(list.Get(), live.giRaw.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (live.giValid)
                Transition(list.Get(), live.giPrev.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER gb = {};
            gb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            gb.UAV.pResource = live.giBuf.Get();
            list->ResourceBarrier(1, &gb);
        }
    }
    live.giValid = giNow && !nrdActive && !nrdDone && live.shadowRawBuf && live.denoisePso;
    live.giStamp = live.frame;
    // recherche de la convention des vecteurs de mouvement (tant qu'elle n'est pas trouvée)
    if (motionOk && live.mvConvention == 255 && live.mvcalPso)
    {
        if (!live.mvErrors)
        {
            live.mvErrors = UavBuffer(g.device, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (auto& r : live.mvErrorsReadback)
                r = MakeBuffer(g.device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (live.mvErrors)
        {
            Transition(list.Get(), live.mvErrors.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(live.mvErrors.Get(), 0, g.zeros.Get(), 0, 256);
            Transition(list.Get(), live.mvErrors.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetComputeRootSignature(live.mvcalRs.Get());
            list->SetPipelineState(live.mvcalPso.Get());
            list->SetComputeRootShaderResourceView(0, live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.depthPrevBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, live.mvBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, live.mvErrors->GetGPUVirtualAddress());
            const uint32_t mc[8] = { live.width, live.height, live.depthPitch, live.mvPitch, d24 ? 1u : 0u, 0, 0, 0 };
            list->SetComputeRoot32BitConstants(5, 8, mc, 0);
            list->Dispatch(16, 1, 1); // 4096 échantillons
            Transition(list.Get(), live.mvErrors.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(live.mvErrorsReadback[live.frame % 3].Get(), 0, live.mvErrors.Get(), 0, 256);
            Transition(list.Get(), live.mvErrors.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    if (motionOk)
    {
        Transition(list.Get(), live.mvBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(list.Get(), live.depthPrevBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (live.prevShadowBuf)
        Transition(list.Get(), live.prevShadowBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    if (atlasValid)
        Transition(list.Get(), live.atlasBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(live.sunReadback[live.frame % 3].Get(), 0, live.sunBuf.Get(), 0, kLiveSunBytes);
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    LiveTime(list.Get(), 4);
    // reflets : plus de réglage dans le menu (force 1, pas de sol mouillé) ; urti.ini garde la main pour les essais
    {
        int32_t view = live.iniDebugView;
        if (view < 0)
        {
            view = 0;
            if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiDebug"); v.handle)
                runtime->get_uniform_value_int(v, &view, 1);
        }
        if (uint32_t(view) != live.debugView)
            Log("URTI direct : vue de diagnostic %d", view);
        live.debugView = uint32_t(std::clamp(view, 0, 10));
    }
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiGenerated"); v.handle)
    {
        int32_t m = 0;
        runtime->get_uniform_value_int(v, &m, 1);
        live.fgMode = uint32_t(std::clamp(m, 0, 2));
    }
    live.reflEffective = live.reflStrength;
    const float smoothMinEffective = live.smoothMin;
    // 4b. reflets : un rayon réfléchi par pixel lisse (matériau du G-buffer copié dans cette image)
    if (live.reflEffective > 0 || live.debugReflect)
    {
        const uint64_t bytes = uint64_t(live.width) * live.height * 16;
        if (!live.reflectBuf || live.reflectBytes < bytes)
        {
            Retire(live.reflectBuf);
            live.reflectBuf = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.reflectBytes = bytes;
        }
        const bool material = live.materialBuf && live.materialFrame != ~0ull && live.frame <= live.materialFrame + 2 &&
                              live.materialWidth >= live.width && live.materialHeight >= live.height;
        if (live.anchorBytes < bytes)
        {
            for (auto& a : live.anchor)
                Retire(a), a = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.anchorBytes = bytes;
            live.anchorOut = -1;
        }
        if (live.reflectBuf && live.reflectPso && live.anchor[0] && live.anchor[1])
        {
            // ancrage de cette image : l'autre tampon garde celui de la précédente
            live.anchorOut = live.anchorOut < 0 ? 0 : 1 - live.anchorOut;
            live.anchorPrevOk = live.anchorFrame != ~0ull && live.anchorFrame + 1 == live.frame;
            live.anchorFrame = live.frame;
            if (live.materialBuf)
                Transition(list.Get(), live.materialBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
            Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            // cache de surfaces : lu seulement s'il couvre toute la scène et a déjà été rempli
            const bool surfRead = live.surfCacheOn && live.surfCache && live.surfFilled > 0 && live.surfTriangles >= live.capacity / 3;
            if (surfRead)
                Transition(list.Get(), live.surfCache.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            list->SetComputeRootSignature(live.reflectRs.Get());
            list->SetPipelineState(live.reflectPso.Get());
            list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(1, live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, live.materialBuf ? live.materialBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(4, sceneTable->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(5, linksBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(6, live.view->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(7, live.viewPrev ? live.viewPrev->GetGPUVirtualAddress() : live.view->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(8, surfRead ? live.surfCache->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(9, live.transpBuilt ? live.viewT->GetGPUVirtualAddress() : live.view->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(10, live.reflectBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(11, live.anchor[live.anchorOut]->GetGPUVirtualAddress());
            const struct
            {
                uint32_t width, height, depthPitch, materialPitch;
                float A, B, p00, p11, clearDepth;
                uint32_t d24, channel;
                float smoothMin, bias;
                uint32_t flags;
                float f0;
                uint32_t reflectPitch, regionCount, anchors, prevVertexCount, cacheTriangles;
            } rc = { live.width, live.height, live.depthPitch, live.materialPitch, live.A, live.B, live.p00, live.p11, g.depthReversed ? 0.0f : 1.0f,
                     d24 ? 1u : 0u, live.reflChannel, smoothMinEffective, 0.002f, (material ? 1u : 0u) | (live.reflInvert ? 2u : 0u) | (live.reflEverywhere ? 4u : 0u) | (live.debugView == 7 ? 8u : 0u) |
                     (live.debugView == 4 || live.debugReflect == 1 ? 16u : 0u) | (live.alphaScreen ? 32u : 0u) | (surfRead ? 64u : 0u) |
                         (live.transpBuilt ? 256u : 0u), live.f0, live.width * 16,
                     uint32_t(tableNow.size()), live.viewPrev && live.prevValid ? 1u : 0u, uint32_t(live.capacity),
                     surfRead ? uint32_t(live.surfTriangles) : 0u };
            list->SetComputeRoot32BitConstants(12, 20, &rc, 0);
            list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
            if (surfRead)
                Transition(list.Get(), live.surfCache.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // scène de cette image : le cache sera rempli à la composition, sur l'image présentée
            live.sceneTable = sceneTable, live.sceneCount = uint32_t(tableNow.size()), live.sceneStamp = live.frame;
            Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (live.materialBuf)
                Transition(list.Get(), live.materialBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER uavb = {};
            uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavb.UAV.pResource = live.reflectBuf.Get();
            list->ResourceBarrier(1, &uavb);
        }
    }

    LiveTime(list.Get(), 5);
    if (live.timeHeap && live.timeReadback[live.frame % 3])
        list->ResolveQueryData(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (live.frame % 3) * 8, 6, live.timeReadback[live.frame % 3].Get(), 0);
    // 5. image d'ombres pour l'effet
    Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list.Get(), live.mask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = live.mask.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = live.shadowBuf.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, live.width, live.height, 1, live.shadowPitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Transition(list.Get(), live.mask.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // sommets de cette image gardés pour la suivante (mémoire géométrique)
    if (live.viewPrev && live.anchorAlpha > 0)
    {
        Transition(list.Get(), live.view.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list.Get(), live.viewPrev.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.viewPrev.Get(), 0, live.view.Get(), 0, std::min<uint64_t>(cursorNow / 16, live.capacity) * 12);
        Transition(list.Get(), live.viewPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
        Transition(list.Get(), live.view.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.prevStart.swap(live.pendingStart);
        live.prevValid = true;
    }
    else
    {
        Transition(list.Get(), live.view.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.prevValid = false;
    }
    Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);

    // 6. compteurs : lus plus tard, puis remis à zéro pour l'image suivante
    Transition(list.Get(), g.counters.Get(), srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(live.counterReadback[live.frame % 3].Get(), 0, g.counters.Get(), 0, kSlots * 8ull);
    Transition(list.Get(), g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    zeroCounters(D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list.Get(), g.so.Get(), srv, D3D12_RESOURCE_STATE_STREAM_OUT);
    for (uint32_t s = 0; s < kSlots; s++)
        g.slotDraws[s] = 0;

    live.maskFrame = live.frame;
    if (active.handle)
        runtime->set_uniform_value_bool(active, true);
    const effect_uniform_variable showMask = runtime->find_uniform_variable("urti.fx", "UrtiShowMask");
    // urti.ini show_mask=1 coche « masque seul » dans l'effet ; retiré, il la décoche (sinon la vue reste)
    if (showMask.handle && g_liveShowMask != live.iniMaskSet)
        runtime->set_uniform_value_bool(showMask, g_liveShowMask), live.iniMaskSet = g_liveShowMask;
    if (!live.bound && active.handle)
    {
        live.bound = true;
        live.boundFrame = live.frame;
        Log("URTI direct : première image : flux %d, %u dessins, %llu sommets remplis (structure %llu), profondeur %ux%u format %d, uniforme %s",
            slot, drawsThisFrame, (unsigned long long)live.lastFilled, (unsigned long long)live.capacity, live.width, live.height,
            (int)depthDesc.Format, active.handle ? "trouvé" : "ABSENT");
    }
    live.frame++;
}

// ombres RT posées sur l'image présentée, sur la liste immédiate de ReShade (exécutée avant la
// présentation). Toutes les présentations passent ici, y compris les images générées par la génération
// d'images (DLSS-G / FSR), qui sont fabriquées à partir de l'image du jeu avant ReShade : sans cela,
// une image sur deux n'aurait pas les ombres RT.
void LiveComposite(effect_runtime* runtime)
{
    live.composedNow = false;
    if (live.showGameOnly)
        return; // F4 : avant (le calcul RT continue : le retour à « après » est immédiat)
    if (!live.enabled || !live.depthReady || !live.bound || !live.shadowBuf || !live.compositePso)
        return;
    // masque d'une image trop ancienne (calcul sauté : étalonnage, profondeur absente…) : rien plutôt
    // que des ombres figées qui ne suivent plus la caméra
    if (live.maskFrame == ~0ull || live.frame > live.maskFrame + 2)
        return;
    if (live.overlayOpen)
        return; // menu de ReShade ouvert : il doit rester lisible (la passe le recouvrirait)
    if (live.fgMode == 1 && live.imageGenerated)
        return; // image générée laissée telle quelle (réglage du menu)
    const effect_technique technique = runtime->find_technique("urti.fx", "URTI");
    const bool techniqueOn = runtime->get_effects_state() && (!technique.handle || runtime->get_technique_state(technique));
    if (techniqueOn != live.lastTechnique)
        Log("URTI direct : %s dans ReShade", techniqueOn ? "effet URTI actif" : "effet URTI coupé (ou effets coupés) : pas d'ombres RT");
    live.lastTechnique = techniqueOn;
    if (!techniqueOn)
        return;
    const resource back = runtime->get_current_back_buffer();
    if (!back.handle)
        return;
    ID3D12Resource* bb = reinterpret_cast<ID3D12Resource*>(back.handle);
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    uint32_t format = 0;
    switch (bd.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: format = 0; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: format = 1; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: format = 2; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: format = 3; break;
    default: return;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    if (!live.imageIn || live.imageBytes < total)
    {
        Retire(live.imageIn), Retire(live.imageOut);
        live.imageIn = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
        live.imageOut = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        live.imageBytes = total;
        if (!live.imageIn || !live.imageOut)
            return;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(reinterpret_cast<IUnknown*>(runtime->get_command_queue()->get_immediate_command_list()->get_native())->QueryInterface(IID_PPV_ARGS(&list))))
        return;
    // photos de l'écran avant l'interface : tampons (même disposition que imageIn), images d'écran connues,
    // choix d'après les comparaisons lues trois présentations plus tard
    // (et nouvelle disposition dès que l'image d'écran change de format ou de taille : passage en HDR, Alt+Tab)
    const bool snapLayoutChanged = g_liveSnapFp.Footprint.Format != fp.Footprint.Format || g_liveSnapFp.Footprint.Width != fp.Footprint.Width ||
                                   g_liveSnapFp.Footprint.Height != fp.Footprint.Height || g_liveSnapFp.Footprint.RowPitch != fp.Footprint.RowPitch;
    if (live.snapBytes < total || snapLayoutChanged)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (live.snapBytes < total)
        {
            for (uint32_t k = 0; k < kLiveSnaps; k++)
            {
                Retire(live.snaps[k]);
                live.snaps[k] = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_COPY_DEST);
                g_liveSnap[k] = live.snaps[k].Get();
            }
            Retire(live.preUiBuf);
            live.preUiBuf = UavBuffer(g.device, total, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.snapBytes = total;
        }
        if (snapLayoutChanged && g_liveSnapFp.Footprint.Width)
            Log("URTI direct : image d'écran changée (format %d, %ux%u) : photos d'avant l'interface remises à zéro", int(fp.Footprint.Format),
                fp.Footprint.Width, fp.Footprint.Height);
        g_liveSnapFp = fp;
        live.uiChoice = -1;
    }
    {
        // images d'écran de cette présentation seulement : après une recréation, les anciennes sont détruites et
        // leur adresse peut servir à une autre image du jeu
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveBackBuffers.clear();
        for (uint32_t i = 0; i < runtime->get_back_buffer_count(); i++)
            g_liveBackBuffers.insert(runtime->get_back_buffer(i).handle);
    }
    if (!live.uiCounts)
    {
        live.uiCounts = UavBuffer(g.device, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto& r : live.uiReadback)
            r = MakeBuffer(g.device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    const uint32_t uiSlot = uint32_t(live.uiFrame % 3);
    if (const uint32_t* c = nullptr; live.uiFrame >= 3 && live.uiCountAt[uiSlot] && SUCCEEDED(live.uiReadback[uiSlot]->Map(0, nullptr, (void**)&c)) && c)
    {
        // choix fait par live_uiselect sur le GPU, dans la même image (octets 64 et 68 selon la parité)
        const int choice = int(c[16 + live.uiParity[uiSlot]]) - 1;
        const float best = choice >= 0 && choice < 4 && c[8] ? float(c[choice]) / float(c[8]) : 0.0f;
        if (choice != live.uiChoice)
        {
            char detail[160] = "";
            for (uint32_t k = 0, n = 0; k < live.uiCountAt[uiSlot] && k < 4 && c[8]; k++)
                n += std::snprintf(detail + n, sizeof(detail) - n, " ; photo %u : %.1f %% / %.1f %%", k, 100.0 * c[k] / c[8], 100.0 * c[4 + k] / c[8]);
            Log(choice >= 0 ? "URTI direct : image avant l'interface du jeu : photo %d (%.1f %% des pixels identiques à l'image finale)%s"
                            : "URTI direct : pas de photo avant l'interface du jeu (%d, %.1f)%s (identiques à l'image finale / à la précédente)",
                choice, best * 100.0f, detail);
        }
        live.uiChoice = choice;
        live.uiReadback[uiSlot]->Unmap(0, nullptr);
    }
    uint32_t snapCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snapCount = std::min(g_liveSnapCount, kLiveSnaps);
    }
    // réglages de l'effet : écran partagé (F5), masque seul (contrôle) ; plus de force dans le menu : la
    // lumière du soleil est retirée entièrement, comme dans les ombres du jeu
    const float strength = 1.0f;
    bool split = false, showMask = false;
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiSplit"); v.handle)
        runtime->get_uniform_value_bool(v, &split, 1);
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiShowMask"); v.handle)
        runtime->get_uniform_value_bool(v, &showMask, 1);
    showMask = showMask || g_liveShowMask || (live.debugView >= 1 && live.debugView <= 3);
    if (strength != live.lastStrength || split != live.lastSplit || showMask != live.lastShowMask)
        Log("URTI direct : réglages du menu : force %.2f, écran partagé %d, masque seul %d", strength, split, showMask);
    live.lastStrength = strength, live.lastSplit = split, live.lastShowMask = showMask;

    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_TEXTURE_COPY_LOCATION tex = {}, buf = {};
    tex.pResource = bb;
    tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buf.PlacedFootprint = fp;
    const uint32_t slotTime = uint32_t((live.frame + 2) % 3); // image dont les effets viennent d'être enregistrés
    if (live.timeHeap)
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 6);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    buf.pResource = live.imageIn.Get();
    list->CopyTextureRegion(&buf, 0, 0, 0, &tex, nullptr);
    Transition(list.Get(), live.imageIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    for (uint32_t k = 0; k < snapCount; k++)
        Transition(list.Get(), live.snaps[k].Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    // image d'avant l'interface : photos de cette image comparées à l'image finale et à l'image affichée
    // précédente, choix et copie sur le GPU (live_uicompare puis live_uiselect), dans la même image
    live.uiCountAt[uiSlot] = 0;
    ID3D12Resource* preUi = nullptr;
    if (snapCount && live.uiPhoto && live.uiPso && live.uiSelPso && live.preUiBuf)
    {
        list->CopyBufferRegion(live.uiCounts.Get(), 0, g.zeros.Get(), 0, 64);
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = live.uiCounts.Get();
        list->ResourceBarrier(1, &ub);
        Transition(list.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        const uint32_t parity = uint32_t(live.uiFrame & 1);
        live.uiParity[uiSlot] = parity;
        const uint32_t uc[8] = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format == 3 ? 8u : 4u, snapCount, parity, 0, 0 };
        list->SetComputeRootSignature(live.uiRs.Get());
        list->SetPipelineState(live.uiPso.Get());
        list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
        for (uint32_t k = 0; k < kLiveSnaps; k++)
            list->SetComputeRootShaderResourceView(1 + k, live.snaps[std::min(k, snapCount - 1)]->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(5, live.imageOut->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(6, live.uiCounts->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(7, 8, uc, 0);
        list->Dispatch((uint32_t(bd.Width) / 4 + 7) / 8, (bd.Height / 4 + 7) / 8, 1);
        Transition(list.Get(), live.imageOut.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &ub); // comptes écrits, lus (et choix gardé) par live_uiselect
        list->SetComputeRootSignature(live.uiSelRs.Get());
        list->SetPipelineState(live.uiSelPso.Get());
        list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
        for (uint32_t k = 0; k < kLiveSnaps; k++)
            list->SetComputeRootShaderResourceView(1 + k, live.snaps[std::min(k, snapCount - 1)]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(5, live.preUiBuf->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(6, live.uiCounts->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(7, 8, uc, 0);
        list->Dispatch((uint32_t(bd.Width) + 7) / 8, (bd.Height + 7) / 8, 1);
        Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(live.uiReadback[uiSlot].Get(), 0, live.uiCounts.Get(), 0, 72);
        Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list.Get(), live.preUiBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        live.uiCountAt[uiSlot] = snapCount;
        preUi = live.preUiBuf.Get();
    }
    live.uiFrame++;
    // pas de photo sûre d'avant l'interface (lu trois images plus tard) : les couleurs des reflets viendraient de
    // l'image finale, menus compris ; les reflets RT s'effacent en douceur (le jeu garde les siens) et reviennent
    // avec la photo
    // (une photo périmée de temps en temps est rattrapée par live_composite : dernière photo sûre ; seulement
    // au-delà de dix de suite, les couleurs gardées deviennent trop vieilles)
    live.uiBadReadbacks = live.uiChoice < 0 ? live.uiBadReadbacks + 1 : 0;
    const bool uiUnsafe = preUi && live.uiBadReadbacks > 10;
    live.uiReflGate = uiUnsafe ? std::max(0.0f, live.uiReflGate - 0.15f) : std::min(1.0f, live.uiReflGate + 0.1f);
    Transition(list.Get(), live.shadowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    // vues 4 à 8 : lissé, poids, couleur reflétée, distance de touche (dans le canal du lissé), confiance
    const uint32_t reflectView = live.debugView == 4 ? 1u : live.debugView == 5 ? 2u : live.debugView == 6 ? 3u : live.debugView == 7 ? 1u :
                                 live.debugView == 8 ? 3u : live.debugView == 9 ? 4u : live.debugView == 10 ? 5u : live.debugReflect;
    const bool reflections = live.reflectBuf && (live.reflEffective > 0 || reflectView);
    if (reflections)
        Transition(list.Get(), live.reflectBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    // reflets stabilisés dans le temps (image réelle : mise à jour de l'historique ; image générée : le
    // dernier résultat est repris tel quel)
    const bool isGenerated = live.lastPresentDraws == 0;
    int reflHistOut = -1;
    if (reflections && live.resolvePso)
    {
        const uint64_t bytes = uint64_t(live.width) * live.height * 16;
        if (!live.reflHist[0] || live.reflHistBytes < bytes)
        {
            Retire(live.reflHist[0]), Retire(live.reflHist[1]);
            live.reflHist[0] = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.reflHist[1] = UavBuffer(g.device, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live.reflHistBytes = bytes, live.reflHistLast = -1;
        }
        if (live.reflHist[0] && live.reflHist[1])
        {
            if (isGenerated && live.reflHistLast >= 0)
                reflHistOut = live.reflHistLast;
            else if (!isGenerated)
            {
                const int prev = live.reflHistLast, cur = prev == 0 ? 1 : 0;
                const bool motionOk = live.mvBuf && live.mvFrame != ~0ull && live.frame <= live.mvFrame + 2 && live.mvWidth >= live.width &&
                                      live.mvHeight >= live.height && live.depthPrevBuf && prev >= 0;
                Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
                if (live.depthPrevBuf)
                    Transition(list.Get(), live.depthPrevBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
                if (live.mvBuf)
                    Transition(list.Get(), live.mvBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
                Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                if (prev >= 0)
                    Transition(list.Get(), live.reflHist[prev].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                list->SetComputeRootSignature(live.resolveRs.Get());
                list->SetPipelineState(live.resolvePso.Get());
                list->SetComputeRootShaderResourceView(0, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(1, live.reflectBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(2, live.reflHist[prev >= 0 ? prev : cur]->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(3, live.mvBuf ? live.mvBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(4, live.depthBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(5, live.depthPrevBuf ? live.depthPrevBuf->GetGPUVirtualAddress() : live.depthBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(6, live.sunBuf->GetGPUVirtualAddress());
                // ancrage (point reflété retrouvé à l'image précédente, identités) : celui de cette image et le précédent
                const bool anchored = live.anchorOut >= 0 && live.anchor[0] && live.anchor[1] && live.anchorFrame + 1 == live.frame; // écrit par les effets de cette image
                if (anchored)
                    for (auto& a : live.anchor)
                        Transition(list.Get(), a.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
                list->SetComputeRootShaderResourceView(7, anchored ? live.anchor[live.anchorOut]->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
                list->SetComputeRootShaderResourceView(8, anchored ? live.anchor[1 - live.anchorOut]->GetGPUVirtualAddress() : live.reflectBuf->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(9, live.reflHist[cur]->GetGPUVirtualAddress());
                const bool d24r = live.depthFormat == DXGI_FORMAT_R24G8_TYPELESS || live.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
                const struct
                {
                    uint32_t width, height, pitch, format, renderWidth, renderHeight, motionPitch, motionConvention, depthPitch, depth24;
                    float alpha, maxLuminance;
                    uint32_t flags;
                    float p00, p11;
                    float alphaMax, gamma, depthTol, anchorAlpha, anchorGamma;
                } rc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.mvPitch,
                         motionOk ? live.mvConvention : 255u, live.depthPitch, d24r ? 1u : 0u, live.reflAlphaMin, 4.0f,
                         (motionOk ? 1u : 0u) | (live.debugView == 8 ? 4u : 0u) | (anchored && live.anchorPrevOk ? 8u : 0u), live.p00, live.p11,
                         live.reflAlphaMax, live.reflGamma, live.reflDepthTol, live.anchorAlpha, live.anchorGamma };
                list->SetComputeRoot32BitConstants(10, 20, &rc, 0);
                list->Dispatch((live.width + 7) / 8, (live.height + 7) / 8, 1);
                // cache de surfaces : un pixel de rendu par bloc 4×4 (live_surfcache), sur la scène RT de cette image
                if (live.surfCacheOn && live.surfPso && live.tlas && live.sceneTable && live.sceneCount && live.sceneStamp + 1 == live.frame &&
                    live.capacity)
                {
                    const uint64_t triangles = live.capacity / 3;
                    if (!live.surfCache || live.surfTriangles < triangles)
                    {
                        Retire(live.surfCache);
                        live.surfCache = UavBuffer(g.device, triangles * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // à zéro : vide
                        live.surfTriangles = live.surfCache ? triangles : 0, live.surfFilled = 0;
                        Log("URTI direct : cache de surfaces pour %llu triangles (%.1f Mo)", (unsigned long long)triangles, triangles * 8 / 1048576.0);
                    }
                    if (live.surfCache)
                    {
                        list->SetComputeRootSignature(live.surfRs.Get());
                        list->SetPipelineState(live.surfPso.Get());
                        list->SetComputeRootShaderResourceView(0, live.tlas->GetGPUVirtualAddress());
                        list->SetComputeRootShaderResourceView(1, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
                        list->SetComputeRootShaderResourceView(2, live.depthBuf->GetGPUVirtualAddress());
                        list->SetComputeRootShaderResourceView(3, live.sunBuf->GetGPUVirtualAddress());
                        list->SetComputeRootShaderResourceView(4, live.sceneTable->GetGPUVirtualAddress());
                        list->SetComputeRootUnorderedAccessView(5, live.surfCache->GetGPUVirtualAddress());
                        const struct
                        {
                            uint32_t width, height, pitch, format, renderWidth, renderHeight, depthPitch, depth24;
                            float p00, p11;
                            uint32_t regionCount, frameIndex, triangles, pad0;
                            float uiZone[4];
                        } sc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.depthPitch, d24r ? 1u : 0u,
                                 live.p00, live.p11, live.sceneCount, uint32_t(live.frame), uint32_t(live.surfTriangles), 0,
                                 { live.uiZone[0], live.uiZone[1], live.uiZone[2], live.uiZone[3] } };
                        list->SetComputeRoot32BitConstants(6, 18, &sc, 0);
                        list->Dispatch((live.width / 4 + 8) / 8, (live.height / 4 + 8) / 8, 1);
                        D3D12_RESOURCE_BARRIER cb = {};
                        cb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        cb.UAV.pResource = live.surfCache.Get();
                        list->ResourceBarrier(1, &cb);
                        live.surfFilled++;
                    }
                }
                if (anchored)
                    for (auto& a : live.anchor)
                        Transition(list.Get(), a.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (prev >= 0)
                    Transition(list.Get(), live.reflHist[prev].Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (live.mvBuf)
                    Transition(list.Get(), live.mvBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
                if (live.depthPrevBuf)
                    Transition(list.Get(), live.depthPrevBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
                Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_RESOURCE_BARRIER ub = {};
                ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                ub.UAV.pResource = live.reflHist[cur].Get();
                list->ResourceBarrier(1, &ub);
                live.reflHistLast = cur;
                reflHistOut = cur;
            }
        }
        if (reflHistOut >= 0)
            Transition(list.Get(), live.reflHist[reflHistOut].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    }
    // part du soleil dans l'image du jeu (live_sunlight) : points d'une même surface que le jeu éclaire et ombre
    if (live.sunMeasure && !isGenerated && live.sunlightPso && live.atlasBuf && live.atlasFrame + 1 == live.frame && live.depthBuf && // copiée par les effets de cette image
        live.calibrated && live.framesSinceSun < 30)
    {
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(live.sunBuf.Get(), 1232, g.zeros.Get(), 0, kLiveSunBytes - 1232);
        Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list.Get(), live.depthBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
        Transition(list.Get(), live.atlasBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
        list->SetComputeRootSignature(live.sunlightRs.Get());
        list->SetPipelineState(live.sunlightPso.Get());
        list->SetComputeRootShaderResourceView(0, live.depthBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, live.atlasBuf->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3, live.sunBuf->GetGPUVirtualAddress());
        const bool d24s = live.depthFormat == DXGI_FORMAT_R24G8_TYPELESS || live.depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT;
        const struct
        {
            uint32_t width, height, depthPitch, depth24;
            float A, B, p00, p11, sunSign, gameBias;
            uint32_t atlasPitch, atlasFormat, imageWidth, imageHeight, imagePitch, format, frameIndex, pad0, pad1, pad2;
        } lc = { live.width, live.height, live.depthPitch, d24s ? 1u : 0u, live.A, live.B, live.p00, live.p11, live.sunSign, g_liveGameBias,
                 live.atlasPitch, live.atlas16 ? 2u : live.atlasD24 ? 1u : 0u, uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, uint32_t(live.frame), 0, 0, 0 };
        list->SetComputeRoot32BitConstants(4, 20, &lc, 0);
        list->Dispatch((live.width / 4 + 7) / 8, (live.height / 4 + 7) / 8, 1);
        Transition(list.Get(), live.atlasBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(list.Get(), live.depthBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_BARRIER ub = {};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = live.sunBuf.Get();
        list->ResourceBarrier(1, &ub);
    }
    list->SetComputeRootSignature(live.compositeRs.Get());
    list->SetPipelineState(live.compositePso.Get());
    list->SetComputeRootShaderResourceView(0, live.imageIn->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, reflections ? live.reflectBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    // image générée : fabriquée entre la vraie image précédente et celle dont le calcul est le dernier fait (le
    // calcul a lieu dès que les dessins du jeu sont exécutés : avec DLSS-G, juste avant l'image générée) ; mode
    // mélange : moitié de chaque
    const bool generated = live.imageGenerated;
    const bool blend = generated && live.fgMode == 2 && live.prevShadowBuf;
    if (blend)
        Transition(list.Get(), live.prevShadowBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(3, blend ? live.prevShadowBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(4, reflHistOut >= 0 ? live.reflHist[reflHistOut]->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(5, preUi ? preUi->GetGPUVirtualAddress() : live.imageIn->GetGPUVirtualAddress());
    Transition(list.Get(), live.sunBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootShaderResourceView(6, live.sunBuf->GetGPUVirtualAddress());
    if (live.uiCounts)
        Transition(list.Get(), live.uiCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
    list->SetComputeRootShaderResourceView(7, live.uiCounts ? live.uiCounts->GetGPUVirtualAddress() : live.sunBuf->GetGPUVirtualAddress());
    // rebond de lumière de cette image et couleur propre des surfaces (copiée pendant l'image du jeu)
    // rebond de lumière : celui de ce calcul, ou le dernier calculé quand rien de neuf n'a été calculé (image générée)
    const bool giFresh = live.giStamp + 1 == live.frame || (live.lastPresentDraws == 0 && live.giStamp + 2 == live.frame);
    const bool giUse = live.giStrength > 0 && live.giValid && giFresh && live.albedoBuf && live.albedoFrame != ~0ull &&
                       live.frame <= live.albedoFrame + 3 && live.albedoWidth >= live.width && live.albedoHeight >= live.height;
    const bool normalUse = giUse && live.personMask && live.normalBuf && live.normalFrame != ~0ull && live.frame <= live.normalFrame + 3 && live.normalWidth >= live.width &&
                           live.normalHeight >= live.height;
    if (giUse)
    {
        Transition(list.Get(), live.giBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        Transition(list.Get(), live.albedoBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    }
    if (normalUse)
        Transition(list.Get(), live.normalBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv);
    list->SetComputeRootShaderResourceView(8, giUse ? live.giBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(9, giUse ? live.albedoBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(10, normalUse ? live.normalBuf->GetGPUVirtualAddress() : live.shadowBuf->GetGPUVirtualAddress());
    if (const uint64_t maskBytes = uint64_t(bd.Width) * bd.Height * 4; !live.uiMaskBuf || live.uiMaskBytes < maskBytes)
        Retire(live.uiMaskBuf), live.uiMaskBuf = UavBuffer(g.device, maskBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), live.uiMaskBytes = maskBytes;
    list->SetComputeRootUnorderedAccessView(11, live.imageOut->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(12, live.uiMaskBuf ? live.uiMaskBuf->GetGPUVirtualAddress() : live.imageOut->GetGPUVirtualAddress());
    const bool tinted = live.sunMeasure && live.sunSkyFrames && live.sunSky[3] > 0.01f; // teinte des ombres mesurée
    const struct
    {
        uint32_t width, height, pitch, format, maskWidth, maskHeight, maskPitch;
        float strength;
        uint32_t split, showMask;
        float reflStrength;
        uint32_t debugReflect;
        float p00, p11;
        float tint[3];
        uint32_t uiParity;
        float uiZone[4];
        float giStrength;
        uint32_t albedoPitch, normalPitch;
    } cc = { uint32_t(bd.Width), bd.Height, fp.Footprint.RowPitch, format, live.width, live.height, live.shadowPitch, strength,
             split ? 1u : 0u, showMask ? 1u : 0u,
             (reflections && !(generated && live.fgMode == 2)) ? (reflectView ? 1.0f : live.reflEffective * live.uiReflGate) : 0.0f,
             (reflections ? reflectView : 0u) | (blend ? 0x100u : 0u) | (reflHistOut >= 0 ? 0x200u : 0u) | (preUi ? 0x400u : 0u) |
             (live.sunShadows ? 0u : 0x800u), live.p00, live.p11,
             { tinted ? live.sunSky[0] / live.sunSky[3] : 1.0f, tinted ? live.sunSky[1] / live.sunSky[3] : 1.0f, tinted ? live.sunSky[2] / live.sunSky[3] : 1.0f },
             uint32_t((live.uiFrame - 1) & 1), { live.uiZone[0], live.uiZone[1], live.uiZone[2], live.uiZone[3] },
             giUse ? live.giStrength : 0.0f, live.albedoPitch, normalUse ? live.normalPitch : 0u };
    list->SetComputeRoot32BitConstants(13, 25, &cc, 0);
    list->Dispatch((cc.width + 7) / 8, (cc.height + 7) / 8, 1);
    if (giUse)
    {
        Transition(list.Get(), live.giBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list.Get(), live.albedoBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (normalUse)
        Transition(list.Get(), live.normalBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list.Get(), live.sunBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (live.uiCounts)
        Transition(list.Get(), live.uiCounts.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list.Get(), live.shadowBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (blend)
        Transition(list.Get(), live.prevShadowBuf.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    if (reflHistOut >= 0)
        Transition(list.Get(), live.reflHist[reflHistOut].Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (reflections)
        Transition(list.Get(), live.reflectBuf.Get(), srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list.Get(), live.imageIn.Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    for (uint32_t k = 0; k < snapCount; k++)
        Transition(list.Get(), live.snaps[k].Get(), srv, D3D12_RESOURCE_STATE_COPY_DEST);
    if (preUi)
        Transition(list.Get(), preUi, srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    buf.pResource = live.imageOut.Get();
    list->CopyTextureRegion(&tex, 0, 0, 0, &buf, nullptr);
    Transition(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Transition(list.Get(), live.imageOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (live.timeHeap && live.timeReadback[slotTime])
    {
        list->EndQuery(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 7);
        list->ResolveQueryData(live.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotTime * 8 + 6, 2, live.timeReadback[slotTime].Get(), 6 * 8);
    }
    live.statComposited++;
    live.composedNow = true;
}

bool OnOpenOverlay(effect_runtime*, bool open, input_source)
{
    live.overlayOpen = open;
    return false;
}

// photo de l'écran après les effets (fichier déclencheur urti_photo, ou présentations listées dans urti.ini
// photo_frames= : urti_photo_<présentation>.ppm, pour comparer deux lancements image par image)
void OnReshadePresent(effect_runtime* runtime)
{
    if (!OurDevice(runtime->get_device()))
        return;
    // case « URTI activé » du menu : décochée, plus aucun calcul RT (capture, structure, ombres, reflets, pose) ;
    // F7 fait la même chose et la case suit
    if (const effect_uniform_variable v = runtime->find_uniform_variable("urti.fx", "UrtiOn"); v.handle && g_liveAtStart)
    {
        bool on = true;
        runtime->get_uniform_value_bool(v, &on, 1);
        if (int(on) != live.menuOn)
        {
            if (live.menuOn >= 0 && live.enabled != on)
                Log("URTI : %s (menu)", on ? "activé, le calcul ray tracing reprend" : "coupé, plus aucun calcul ray tracing");
            live.menuOn = int(on), live.enabled = on;
        }
        else if (live.enabled != on)
            runtime->set_uniform_value_bool(v, live.enabled), live.menuOn = int(live.enabled); // F7
    }
    LiveComposite(runtime);
    // rafale : l'image sans URTI (sa copie, imageIn) et avec, à chaque image réelle composée
    if (live.burst > 0 && live.composedNow && live.lastPresentDraws > 0 && live.imageIn)
        if (const resource back = runtime->get_current_back_buffer(); back.handle)
        {
            runtime->get_command_queue()->flush_immediate_command_list();
            ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
            const std::string base = live.burstFolder + "\\";
            SaveBackBuffer(queue, reinterpret_cast<ID3D12Resource*>(back.handle), base + "apres_" + std::to_string(live.burstIndex) + ".ppm", 2);
            SaveBackBuffer(queue, reinterpret_cast<ID3D12Resource*>(back.handle), base + "avant_" + std::to_string(live.burstIndex) + ".ppm", 2, live.imageIn.Get());
            // photo d'avant l'interface : la première prise dans cette image (brute) et celle qu'URTI a gardée
            if (live.snaps[0])
                SaveBackBuffer(queue, reinterpret_cast<ID3D12Resource*>(back.handle), base + "photo_" + std::to_string(live.burstIndex) + ".ppm", 2, live.snaps[0].Get());
            if (live.preUiBuf)
                SaveBackBuffer(queue, reinterpret_cast<ID3D12Resource*>(back.handle), base + "gardee_" + std::to_string(live.burstIndex) + ".ppm", 2, live.preUiBuf.Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Log("RAFALE %u %u image %llu objets RT %zu entrés %u sortis %u", live.burstCount, live.burstIndex, (unsigned long long)live.frame, live.admitted.size(), live.churnIn,
                live.churnOut);
            live.burstIndex++;
            if (--live.burst == 0)
                Log("URTI direct : rafale %u terminée (%u paires d'images)", live.burstCount, live.burstIndex);
        }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveSnapCount = 0; // photos de la prochaine image
    }
    if (++live.statPresents == 300)
    {
        Log("URTI direct : sur 300 présentations, %u sans dessin du jeu, %u passages dans les effets, %u avec ombres RT appliquées",
            live.statNoDraw, live.statEffects, live.statComposited);
        Log("URTI direct : dessins exécutés : %u écrits dans la même fenêtre de présentation (%u listes), %u écrits avant la présentation "
            "précédente (%u listes)", g_execOnTime.exchange(0), g_execLists.exchange(0), g_execLate.exchange(0), g_execLateLists.exchange(0));
        live.statPresents = live.statNoDraw = live.statEffects = live.statComposited = 0;
    }
    if (std::binary_search(g_photoFrames.begin(), g_photoFrames.end(), g.frame))
    {
        if (const resource back = runtime->get_current_back_buffer(); back.handle)
        {
            runtime->get_command_queue()->flush_immediate_command_list();
            SaveBackBuffer(reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()), reinterpret_cast<ID3D12Resource*>(back.handle),
                           g_dir + "\\urti_photo_" + std::to_string(g.frame) + ".ppm", 1);
        }
        return;
    }
    const std::string trigger = g_dir + "\\urti_photo";
    // pas avant que ReShade ait fini de charger ses effets (quelques dixièmes de seconde)
    if (g.frame < 300 || g.frame % 15 != 0 || GetFileAttributesA(trigger.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    if (live.enabled && (!live.bound || live.frame < live.boundFrame + 60))
        return; // ombres pas encore prêtes : la photo attend
    DeleteFileA(trigger.c_str());
    // lecture directe du tampon d'affichage (gère le HDR10 du jeu, que capture_screenshot rend brut)
    const resource back = runtime->get_current_back_buffer();
    if (!back.handle)
        return;
    const std::string path = g_dir + "\\urti_photo" + std::to_string(live.photoIndex++) + ".ppm";
    runtime->get_command_queue()->flush_immediate_command_list(); // les effets de cette image d'abord
    SaveBackBuffer(reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()), reinterpret_cast<ID3D12Resource*>(back.handle), path, 1);
    Log("URTI : photo %s (ombres RT %s)", path.c_str(), live.enabled ? "actives" : "coupées");
}

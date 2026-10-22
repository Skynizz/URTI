// ============================================================================================
// Pipeline « comme en jeu » : image finale par pixel, puis débruiteur commun.
//  - KRC : éclairage direct par pixel (une estimation directe par frame) + lumière indirecte
//    lue dans les cellules (qui ne stockent alors que l'indirect, F_INDIRECT) ;
//  - SHaRC : sa passe Render officielle (sharc_render, sharc.wgsl).
// Débruiteur (identique pour les deux) : SVGF simplifié. Accumulation temporelle de
// l'illumination démodulée (÷ albédo) avec reprojection par la position monde, puis trois
// filtres à-trous 5×5 (pas 1, 2, 4) guidés par la normale et la position, remodulation.
// ============================================================================================

@group(0) @binding(28) var<storage, read_write> dn_hist: array<vec4<f32>>;  // illumination accumulée, w : longueur d'historique
@group(0) @binding(29) var<storage, read_write> dn_t1: array<vec4<f32>>;
@group(0) @binding(30) var<storage, read_write> dn_t2: array<vec4<f32>>;
@group(0) @binding(31) var<storage, read_write> dn_prevg: array<vec4<f32>>; // frame précédente : (position, genre), (normale, -)

// partie spéculaire de l'image (le débruiteur la traite à part : pas de démodulation par l'albédo
// diffus, historique court, peu de filtrage spatial pour garder les reflets nets)
@group(0) @binding(35) var<storage, read_write> spec_img: array<vec4<f32>>;
@group(0) @binding(36) var<storage, read_write> spec_hist: array<vec4<f32>>;
@group(0) @binding(37) var<storage, read_write> spec_t: array<vec4<f32>>;

const DN_HIST_MAX: f32 = 32.0;
const DN_SPEC_HIST_MAX: f32 = 4.0;
const DN_SPEC_GAMMA: f32 = 2.0;

// plancher bas : à 0,05, les canaux sombres d'une texture saturée (rouge : vert et bleu ≈ 0,02)
// gardaient la texture dans l'illumination, que le filtre spatial étalait (bordures dorées des
// bannières floues et ternes)
fn dn_demod(a: vec3<f32>) -> vec3<f32> { return max(a, vec3<f32>(0.002)); }
// albédo diffus du pixel (démodulation de la partie diffuse)
fn dn_dalb(g: Gpx) -> vec3<f32> { return dn_demod(diff_albedo(g.albedo, g_metal(g))); }

// Reflets KRC (passe avant le resolve). Un rayon du lobe spéculaire par estimation ; au point
// touché, si une cellule mûre le couvre : sa lumière indirecte (démodulée) × l'albédo exact du
// point (texture nette) + le direct calculé ici. Sinon (point hors champ : décor derrière la
// caméra, sol caché par la sphère), la cellule y est créée et le chemin tracé la nourrit : dès
// les frames suivantes, le reflet est lu dans le cache au lieu d'être un chemin isolé (points
// blancs). Une cellule hors champ mûre reste nourrie par une fraction REFL_FEED des rayons
// (moyenne glissante du resolve), pour suivre les changements de lumière.
const REFL_FEED: f32 = 0.25;
const REFL_N_MIN: f32 = 4.0;

fn refl_deposit(c: u32, l: vec3<f32>, dl: vec3<f32>, gda: vec3<f32>) {
    var u = l / demod(gda);
    var dd = dl / demod(gda);
    // même écrêtage des lucioles que les échantillons ordinaires
    if ((fr.off & 4u) == 0u && cells[c].n >= 8.0) {
        let kf = select(FIREFLY_K, FIREFLY_K_YOUNG, cells[c].n < 64.0);
        let cap = kf * max(cells[c].lmean, 1e-4);
        let lu = lum(u);
        if (lu > cap) {
            u *= cap / lu;
            dd *= cap / lu;
        }
    }
    let b = c * ACC;
    if (atomicAdd(&acc[b + A_FN], 1u) == 0u && atomicLoad(&acc[b + A_FOOT]) == 0u) {
        vis_list[atomicAdd(&ctr[C_NVIS], 1u)] = c; // résolue avec les cellules visibles
    }
    add_fx(b + A_FR, u.x);
    add_fx(b + A_FR + 1u, u.y);
    add_fx(b + A_FR + 2u, u.z);
    let lu = lum(u);
    add_fx(b + A_FL, lu);
    add_f32(b + A_FL2, lu * lu);
    if ((fr.flags & F_DSPLIT) != 0u) {
        add_fx(b + A_FD, dd.x);
        add_fx(b + A_FD + 1u, dd.y);
        add_fx(b + A_FD + 2u, dd.z);
    }
}

fn refl_sample(g: Gpx, wo: vec3<f32>, rough: f32, metal: f32) -> vec3<f32> {
    let bs = spec_sample(g.n, wo, g.albedo, rough, metal);
    if (!bs.ok) { return vec3<f32>(0.0); }
    let h = intersect(g.p + g.n * OFFSET, bs.wi);
    if (h.found && h.light >= 0 && (fr.off & 32768u) == 0u) {
        // lampe vue dans le reflet (le ciel dans la sphère) : part BSDF du MIS
        mis_mode = 1u;
        let e = mis_light_hit(u32(h.light), h.t, g.n, wo, bs.wi, g.albedo, rough, metal);
        mis_mode = 0u;
        return bs.w * e;
    }
    if (!h.found || h.light >= 0) { return vec3<f32>(0.0); }
    let hd = length(h.p - fr.eye);
    let gda = diff_albedo(h.albedo, h.metal);
    let key = key_at(h.p, h.n, hd, vec2<f32>(rnd() - 0.5, rnd() - 0.5));
    var c = find_cell(key);
    let mature = c != 0xffffffffu && cells[c].n >= REFL_N_MIN;
    var feed = 0xffffffffu;
    if ((fr.off & 4096u) == 0u) {
        if (!mature) {
            feed = insert_key(key);
        } else if (atomicLoad(&acc[c * ACC + A_FOOT]) == 0u && rnd() < REFL_FEED) {
            feed = c;
        }
    }
    var out = vec3<f32>(0.0);
    if (mature) {
        if (h.metal < 0.5 && (fr.off & 8192u) == 0u) {
            // surface non métallique : toute sa lumière (direct compris) lue dans la cellule ; un seul
            // rayon d'ombre par pixel au point reflété faisait des points blancs dans les miroirs
            out = cells[c].mean * demod(gda);
        } else {
            // métal : le spéculaire dépend de la direction, calculé ici ; indirect de la cellule
            let nh = nee_split(h.p, h.n, -bs.wi, h.albedo, reg_rough(h.rough), h.metal);
            let ld = select(cells[c].mean - cells[c].dmean, cells[c].mean, (fr.flags & F_DSPLIT) == 0u);
            out = nh.d + nh.s + max(ld, vec3<f32>(0.0)) * demod(gda);
        }
    }
    if (!mature || feed != 0xffffffffu) {
        pm_wo = -bs.wi;
        pm_rough = reg_rough(h.rough);
        pm_metal = h.metal;
        pm_diffuse_only = false;
        resample = (fr.flags & F_RESAMPLE) != 0u;
        rs_depth = 1u;
        let l = radiance(h.p, h.n, h.albedo).l;
        if (!mature) { out = l; }
        if (feed != 0xffffffffu) { refl_deposit(feed, l, d0_l, gda); }
    }
    return bs.w * out;
}

@compute @workgroup_size(64)
fn krc_refl(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    var sp = vec3<f32>(0.0);
    if (g.kind == 2u && !is_diffuse(g_rough(g), g_metal(g))) {
        rec_on = false;
        uni = false;
        split = false;
        sel = 0u;
        seed_rng(k, 0x5bd1u, fr.frame);
        mis_mode = 0u;
        let wo = normalize(fr.eye - g.p);
        let nspp = max(fr.krc_spp, 1u);
        for (var si = 0u; si < nspp; si++) {
            sp += refl_sample(g, wo, g_rough(g), g_metal(g));
        }
        sp /= f32(nspp);
    }
    spec_img[k] = vec4<f32>(sp, 0.0);
}

// image finale KRC : direct par pixel + indirect des cellules
@compute @workgroup_size(64)
fn krc_final(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    var v = vec3<f32>(0.0);
    if (g.kind == 1u) {
        v = g.albedo;
    } else if (g.kind == 2u) {
        rec_on = false;
        uni = false;
        split = false;
        sel = 0u;
        seed_rng(k, 0x91d3u, fr.frame);
        let rough = g_rough(g);
        let metal = g_metal(g);
        let wo = normalize(fr.eye - g.p);
        // krc_spp estimations par pixel de l'éclairage direct (le cache donne l'indirect diffus, déjà
        // lisse ; le bruit de l'image finale vient du direct par pixel) ; le reflet vient de krc_refl
        let nspp = max(fr.krc_spp, 1u);
        var sp = vec3<f32>(0.0);
        for (var si = 0u; si < nspp; si++) {
            mis_mode = select(1u, 0u, (fr.off & 32768u) != 0u || is_diffuse(rough, metal));
            let ns = nee_split(g.p, g.n, wo, g.albedo, rough, metal);
            mis_mode = 0u;
            v += ns.d;
            sp += ns.s;
        }
        v /= f32(nspp);
        sp /= f32(nspp);
        spec_img[k] = vec4<f32>(spec_img[k].rgb + sp, 0.0);
        // indirect des K cellules du pixel : moyenne tronquée (sans la plus claire ni la plus
        // sombre) dès qu'il y en a 4 ; une cellule aberrante isolée (luciole capturée) n'est
        // alors jamais affichée, une vraie variation partagée par les voisines passe
        var vals: array<vec3<f32>, 4>;
        var nv = 0u;
        for (var s = 0u; s < K; s++) {
            let c = pix_cells[k * K + s];
            if (c != 0xffffffffu && cells[c].n > 0.0 && nv < 4u) {
                if ((fr.flags & F_DSPLIT) != 0u) {
                    vals[nv] = max(cells[c].mean - cells[c].dmean, vec3<f32>(0.0));
                } else {
                    vals[nv] = cells[c].mean;
                }
                nv++;
            }
        }
        var ind = vec3<f32>(0.0);
        var w = 0.0;
        if (nv == 4u && (fr.off & 2u) == 0u) {
            var imin = 0u;
            var imax = 0u;
            for (var s = 1u; s < 4u; s++) {
                if (lum(vals[s]) < lum(vals[imin])) { imin = s; }
                if (lum(vals[s]) >= lum(vals[imax])) { imax = s; }
            }
            if (imin == imax) { imax = (imin + 1u) % 4u; }
            for (var s = 0u; s < 4u; s++) {
                if (s != imin && s != imax) {
                    ind += vals[s];
                    w += 1.0;
                }
            }
        } else {
            for (var s = 0u; s < nv; s++) {
                ind += vals[s];
                w += 1.0;
            }
        }
        if (fr.dbg == 1u) {
            v = vec3<f32>(0.0);
            spec_img[k] = vec4<f32>(0.0);
        }
        if (fr.dbg != 2u && w > 0.0) { v += ind / w * demod(diff_albedo(g.albedo, metal)); }
    } else {
        spec_img[k] = vec4<f32>(0.0);
    }
    image[k] = vec4<f32>(v, f32(g.kind));
}

// anti-lucioles (identique pour les deux méthodes, comme dans les débruiteurs de jeu) : un pixel
// plus de FF_K fois plus lumineux que le plus lumineux de ses 8 voisins est ramené à cette borne
// (à 2×, il retirait 20 à 30 % de la lumière d'une scène éclairée par des valeurs rares)
const FF_K: f32 = 8.0;
fn ff_diffuse(k: u32) -> vec3<f32> {
    let c = image[k].rgb / dn_dalb(gbuf[k]);
    let x = i32(k % fr.w);
    let y = i32(k / fr.w);
    var mx = 0.0;
    for (var dy = -1; dy <= 1; dy++) {
        for (var dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) { continue; }
            let qx = x + dx;
            let qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= i32(fr.w) || qy >= i32(fr.h)) { continue; }
            let qi = u32(qy) * fr.w + u32(qx);
            if (gbuf[qi].kind != 2u) { continue; }
            mx = max(mx, lum(image[qi].rgb / dn_dalb(gbuf[qi])));
        }
    }
    let l = lum(c);
    if ((fr.off & 1u) == 0u && l > FF_K * mx && l > 0.0) { return c * (FF_K * mx / l); }
    return c;
}

fn ff_spec(k: u32) -> vec3<f32> {
    let c = spec_img[k].rgb;
    let x = i32(k % fr.w);
    let y = i32(k / fr.w);
    var mx = 0.0;
    for (var dy = -1; dy <= 1; dy++) {
        for (var dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) { continue; }
            let qx = x + dx;
            let qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= i32(fr.w) || qy >= i32(fr.h)) { continue; }
            mx = max(mx, lum(spec_img[u32(qy) * fr.w + u32(qx)].rgb));
        }
    }
    let l = lum(c);
    if ((fr.off & 1u) == 0u && l > FF_K * mx && l > 0.0) { return c * (FF_K * mx / l); }
    return c;
}

// pixel (continu) d'un point monde dans la caméra précédente ; x < 0 si hors champ
fn dn_prev_px(p: vec3<f32>) -> vec2<f32> {
    let d = p - fr.prev_eye;
    let z = dot(d, fr.prev_fwd);
    if (z <= 1e-4) { return vec2<f32>(-1.0); }
    let sx = dot(d, fr.prev_right) / (z * fr.th);
    let sy = dot(d, fr.prev_up) / (z * fr.tv);
    return vec2<f32>((sx + 1.0) * 0.5 * f32(fr.w) - 0.5, (1.0 - sy) * 0.5 * f32(fr.h) - 0.5);
}

// bornage de l'historique du diffus, en écarts-types du voisinage 5×5 (plus large et plus robuste
// qu'un 3×3 : sur une entrée à queue lourde, un bornage serré assombrit)
const DN_GAMMA: f32 = 2.0;
struct Moments { m1: vec3<f32>, m2: vec3<f32> };

// moments de l'illumination courante (démodulée) sur le voisinage 3×3 de surface
fn dn_moments(k: u32) -> Moments {
    var m: Moments;
    m.m1 = vec3<f32>(0.0);
    m.m2 = vec3<f32>(0.0);
    var n = 0.0;
    let x = i32(k % fr.w);
    let y = i32(k / fr.w);
    for (var dy = -2; dy <= 2; dy++) {
        for (var dx = -2; dx <= 2; dx++) {
            let qx = x + dx;
            let qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= i32(fr.w) || qy >= i32(fr.h)) { continue; }
            let qi = u32(qy) * fr.w + u32(qx);
            let gq = gbuf[qi];
            if (gq.kind != 2u) { continue; }
            let v = image[qi].rgb / dn_dalb(gq);
            m.m1 += v;
            m.m2 += v * v;
            n += 1.0;
        }
    }
    m.m1 /= max(n, 1.0);
    m.m2 /= max(n, 1.0);
    return m;
}

@compute @workgroup_size(64)
fn dn_temporal(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    if (g.kind != 2u) {
        dn_t1[k] = vec4<f32>(image[k].rgb, 0.0);
        return;
    }
    let cur = ff_diffuse(k);
    var out = vec4<f32>(cur, 1.0);
    if ((fr.off & 16u) != 0u) {
        dn_t1[k] = out;
        return;
    }
    let q = dn_prev_px(g.p);
    if (q.x >= 0.0) {
        let i = i32(round(q.x));
        let j = i32(round(q.y));
        if (i >= 0 && j >= 0 && i < i32(fr.w) && j < i32(fr.h)) {
            let idx = u32(j) * fr.w + u32(i);
            let pg = dn_prevg[2u * idx];
            let pn = dn_prevg[2u * idx + 1u].xyz;
            let tol = max(0.02, 4.0 * g.dist * fr.px_angle);
            if (pg.w == 2.0 && length(pg.xyz - g.p) < tol && dot(pn, g.n) > 0.9) {
                let h = dn_hist[idx];
                let len = min(h.w + 1.0, DN_HIST_MAX);
                // bornage de l'historique par la variance du voisinage 3×3 courant (comme le TAA et
                // A-SVGF) : un changement d'éclairage net rejette l'ancien historique au lieu de le traîner
                let mo = dn_moments(k);
                let sd = sqrt(max(mo.m2 - mo.m1 * mo.m1, vec3<f32>(0.0)));
                let gam = select(DN_GAMMA, fr.dn_gamma, fr.dn_gamma > 0.0);
                let hc = clamp(h.rgb, mo.m1 - gam * sd, mo.m1 + gam * sd);
                // détection de changement (comme A-SVGF) : si la moyenne locale actuelle (25 pixels)
                // s'écarte de l'historique bien au-delà de son incertitude (σ/5), l'éclairage a
                // vraiment changé : historique ramené à 2 frames, l'image suit tout de suite
                var l2 = len;
                let dl = abs(lum(h.rgb) - lum(mo.m1));
                if (dl > 3.0 * lum(sd) / 5.0 + 0.05 * lum(mo.m1)) { l2 = min(len, 2.0); }
                out = vec4<f32>(mix(hc, cur, 1.0 / l2), l2);
            }
        }
    }
    dn_t1[k] = out;
}

// partie spéculaire : accumulation temporelle courte (historique ≤ 4, borné par le voisinage)
@compute @workgroup_size(64)
fn dn_spec(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    let cur = ff_spec(k);
    var out = vec4<f32>(cur, 1.0);
    if (g.kind == 2u && !is_diffuse(g_rough(g), g_metal(g))) {
        let q = dn_prev_px(g.p);
        if (q.x >= 0.0) {
            let i = i32(round(q.x));
            let j = i32(round(q.y));
            if (i >= 0 && j >= 0 && i < i32(fr.w) && j < i32(fr.h)) {
                let idx = u32(j) * fr.w + u32(i);
                let pg = dn_prevg[2u * idx];
                let tol = max(0.02, 4.0 * g.dist * fr.px_angle);
                if (pg.w == 2.0 && length(pg.xyz - g.p) < tol) {
                    var m1 = vec3<f32>(0.0);
                    var m2 = vec3<f32>(0.0);
                    var nn = 0.0;
                    let x = i32(k % fr.w);
                    let y = i32(k / fr.w);
                    for (var dy = -1; dy <= 1; dy++) {
                        for (var dx = -1; dx <= 1; dx++) {
                            let qx = x + dx;
                            let qy = y + dy;
                            if (qx < 0 || qy < 0 || qx >= i32(fr.w) || qy >= i32(fr.h)) { continue; }
                            let sv = spec_img[u32(qy) * fr.w + u32(qx)].rgb;
                            m1 += sv;
                            m2 += sv * sv;
                            nn += 1.0;
                        }
                    }
                    m1 /= nn;
                    m2 /= nn;
                    let sd = sqrt(max(m2 - m1 * m1, vec3<f32>(0.0)));
                    let h = spec_hist[idx];
                    let len = min(h.w + 1.0, DN_SPEC_HIST_MAX);
                    // bornage par le voisinage 3×3 (γ = DN_SPEC_GAMMA) : avec le MIS, les lampes vues en
                    // reflet ne sont plus des valeurs rares, et sans bornage un reflet très lumineux
                    // (le ciel dans la sphère qui roule) traînait en une grande tache blanche
                    var hs = h.rgb;
                    if ((fr.off & 65536u) == 0u) { hs = clamp(hs, m1 - DN_SPEC_GAMMA * sd, m1 + DN_SPEC_GAMMA * sd); }
                    out = vec4<f32>(mix(hs, cur, 1.0 / len), len);
                }
            }
        }
    }
    spec_t[k] = out;
}

@compute @workgroup_size(64)
fn dn_spec_store(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    spec_hist[k] = spec_t[k];
}

// partie spéculaire finale : nette pour les surfaces lisses, moyenne 3×3 (même normale) au-delà
fn dn_spec_out(k: u32) -> vec3<f32> {
    let g = gbuf[k];
    let rough = g_rough(g);
    if ((fr.off & 64u) != 0u) { return spec_img[k].rgb; }
    if (is_diffuse(rough, g_metal(g))) { return vec3<f32>(0.0); }
    if (rough < 0.1) { return spec_t[k].rgb; }
    let x = i32(k % fr.w);
    let y = i32(k / fr.w);
    var acc = vec3<f32>(0.0);
    var ws = 0.0;
    for (var dy = -1; dy <= 1; dy++) {
        for (var dx = -1; dx <= 1; dx++) {
            let qx = x + dx;
            let qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= i32(fr.w) || qy >= i32(fr.h)) { continue; }
            let qi = u32(qy) * fr.w + u32(qx);
            let gq = gbuf[qi];
            if (gq.kind != 2u) { continue; }
            let w = pow(max(dot(g.n, gq.n), 0.0), 32.0);
            acc += w * spec_t[qi].rgb;
            ws += w;
        }
    }
    return acc / max(ws, 1e-6);
}

// historique de la frame suivante, et G-buffer « précédent »
@compute @workgroup_size(64)
fn dn_store(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    dn_hist[k] = dn_t1[k];
    dn_prevg[2u * k] = vec4<f32>(g.p, f32(g.kind));
    dn_prevg[2u * k + 1u] = vec4<f32>(g.n, 0.0);
}

fn dn_load(src: u32, i: u32) -> vec3<f32> {
    if (src == 1u) { return dn_t1[i].rgb; }
    return dn_t2[i].rgb;
}

// filtre à-trous 5×5 (noyau B3), arrêts sur la normale et la position
fn dn_atrous(k: u32, step: i32, src: u32) -> vec3<f32> {
    let g = gbuf[k];
    if (g.kind != 2u || (fr.off & 32u) != 0u) { return dn_load(src, k); }
    var h = array<f32, 5>(1.0 / 16.0, 0.25, 0.375, 0.25, 1.0 / 16.0);
    let x = i32(k % fr.w);
    let y = i32(k / fr.w);
    let sig = max(1e-3, 2.0 * f32(step) * g.dist * fr.px_angle);
    var acc = vec3<f32>(0.0);
    var wsum = 0.0;
    for (var dy = -2; dy <= 2; dy++) {
        for (var dx = -2; dx <= 2; dx++) {
            let qx = x + dx * step;
            let qy = y + dy * step;
            if (qx < 0 || qy < 0 || qx >= i32(fr.w) || qy >= i32(fr.h)) { continue; }
            let qi = u32(qy) * fr.w + u32(qx);
            let gq = gbuf[qi];
            if (gq.kind != 2u) { continue; }
            let wn = pow(max(dot(g.n, gq.n), 0.0), 32.0);
            let wp = exp(-length(g.p - gq.p) / sig);
            let w = h[dx + 2] * h[dy + 2] * wn * wp;
            acc += w * dn_load(src, qi);
            wsum += w;
        }
    }
    return acc / max(wsum, 1e-8);
}

@compute @workgroup_size(64)
fn dn_atrous1(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    dn_t2[k] = vec4<f32>(dn_atrous(k, 1, 1u), 0.0);
}

@compute @workgroup_size(64)
fn dn_atrous2(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    dn_t1[k] = vec4<f32>(dn_atrous(k, 2, 2u), 0.0);
}

// itérations supplémentaires (débruiteur à 5 passages, comme SVGF) : pas 4 et 8, puis 16
@compute @workgroup_size(64)
fn dn_atrous3b(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    dn_t2[k] = vec4<f32>(dn_atrous(k, 4, 1u), 0.0);
}

@compute @workgroup_size(64)
fn dn_atrous4(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    dn_t1[k] = vec4<f32>(dn_atrous(k, 8, 2u), 0.0);
}

@compute @workgroup_size(64)
fn dn_atrous5(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    var v = dn_atrous(k, 16, 1u);
    if (g.kind == 2u) { v = v * dn_dalb(g) + dn_spec_out(k); }
    image[k] = vec4<f32>(v, f32(g.kind));
}

// dernier passage : remodulation par l'albédo, image finale
@compute @workgroup_size(64)
fn dn_atrous3(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    var v = dn_atrous(k, 4, 1u);
    if (g.kind == 2u) { v = v * dn_dalb(g) + dn_spec_out(k); }
    image[k] = vec4<f32>(v, f32(g.kind));
}

// sans débruiteur : image finale = partie diffuse + partie spéculaire
@compute @workgroup_size(64)
fn game_combine(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    if (gbuf[k].kind == 2u) { image[k] = vec4<f32>(image[k].rgb + spec_img[k].rgb, image[k].w); }
}

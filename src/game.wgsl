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

fn dn_demod(a: vec3<f32>) -> vec3<f32> { return max(a, vec3<f32>(0.05)); }
// albédo diffus du pixel (démodulation de la partie diffuse)
fn dn_dalb(g: Gpx) -> vec3<f32> { return dn_demod(diff_albedo(g.albedo, g_metal(g))); }

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
        let ns = nee_split(g.p, g.n, wo, g.albedo, rough, metal);
        v = ns.d;
        var sp = ns.s;
        if (!is_diffuse(rough, metal)) {
            // reflet : un rayon du lobe spéculaire ; au point touché, radiance avec lecture du cache
            let bs = spec_sample(g.n, wo, g.albedo, rough, metal);
            if (bs.ok) {
                let h = intersect(g.p + g.n * OFFSET, bs.wi);
                if (h.found && h.light < 0) {
                    pm_wo = -bs.wi;
                    pm_rough = h.rough;
                    pm_metal = h.metal;
                    pm_diffuse_only = false;
                    resample = (fr.flags & F_RESAMPLE) != 0u;
                    rs_depth = 1u;
                    sp += bs.w * radiance(h.p, h.n, h.albedo).l;
                }
            }
        }
        spec_img[k] = vec4<f32>(sp, 0.0);
        var ind = vec3<f32>(0.0);
        var w = 0.0;
        for (var s = 0u; s < K; s++) {
            let c = pix_cells[k * K + s];
            if (c != 0xffffffffu && cells[c].n > 0.0) {
                if ((fr.flags & F_DSPLIT) != 0u) {
                    ind += max(cells[c].mean - cells[c].dmean, vec3<f32>(0.0));
                } else {
                    ind += cells[c].mean;
                }
                w += 1.0;
            }
        }
        if (w > 0.0) { v += ind / w * demod(diff_albedo(g.albedo, metal)); }
    } else {
        spec_img[k] = vec4<f32>(0.0);
    }
    image[k] = vec4<f32>(v, f32(g.kind));
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

const DN_GAMMA: f32 = 1.5;
struct Moments { m1: vec3<f32>, m2: vec3<f32> };

// moments de l'illumination courante (démodulée) sur le voisinage 3×3 de surface
fn dn_moments(k: u32) -> Moments {
    var m: Moments;
    m.m1 = vec3<f32>(0.0);
    m.m2 = vec3<f32>(0.0);
    var n = 0.0;
    let x = i32(k % fr.w);
    let y = i32(k / fr.w);
    for (var dy = -1; dy <= 1; dy++) {
        for (var dx = -1; dx <= 1; dx++) {
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
    let cur = image[k].rgb / dn_dalb(g);
    var out = vec4<f32>(cur, 1.0);
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
                out = vec4<f32>(mix(hc, cur, 1.0 / len), len);
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
    let cur = spec_img[k].rgb;
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
                    let hc = clamp(h.rgb, m1 - DN_GAMMA * sd, m1 + DN_GAMMA * sd);
                    out = vec4<f32>(mix(hc, cur, 1.0 / len), len);
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
    if (is_diffuse(rough, g_metal(g))) { return vec3<f32>(0.0); }
    if (rough < 0.2) { return spec_t[k].rgb; }
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
    if (g.kind != 2u) { return dn_load(src, k); }
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

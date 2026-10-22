// ============================================================================================
// SHaRC (Spatially Hashed Radiance Cache), portage WGSL de NVIDIA RTXGI SHaRC v1.8.3
// (include/HashGridCommon.h, include/SharcCommon.h, https://github.com/NVIDIA-RTX/SHARC).
//
// This software contains source code provided by NVIDIA Corporation.
// Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
// Distribué sous la NVIDIA RTX SDKs License (third_party/sharc/License.md).
//
// Portage fonction par fonction, réglages par défaut du SDK :
//   HASH_GRID_COMPACT 0 (clés 64 bits, ici deux u32 lo/hi), HASH_GRID_USE_NORMALS 1,
//   HASH_GRID_LIMIT_EMPTY_SLOTS 2 (valeur fixée par SharcCommon.h), seau de 16,
//   HASH_GRID_ENABLE_64_BIT_ATOMICS 0 : la variante à tampon de verrous du SDK (WGSL n'a pas
//   d'atomiques 64 bits), SHARC_UPDATE avec rééchantillonnage du cache (SHARC_PROPAGATION_DEPTH 2),
//   SHARC_BLEND_ADJACENT_LEVELS 1, SHARC_MATERIAL_DEMODULATION 1 (comme l'exemple Pathtracer),
//   pas de SH, pas d'éclairage réactif, pas d'émissif séparé, pas d'accélération du fondu.
// Écarts forcés : float16_t4 → pack2x16float (même stockage 16 bits) ; en cas d'échec du verrou
// après 8 essais, la valeur d'origine est indéfinie en HLSL, ici une sentinelle (= collision).
// ============================================================================================

const HG_POSITION_BIT_NUM: u32 = 17u;
const HG_POSITION_BIT_MASK: u32 = 0x1ffffu;
const HG_LEVEL_BIT_MASK: u32 = 0x1ffu;
const HG_NORMAL_BIT_MASK: u32 = 7u;
const HG_LEVEL_BIT_OFFSET: u32 = 51u;
const HG_NORMAL_BIT_OFFSET: u32 = 60u;
const HG_POSITION_BIAS: f32 = 1e-4;
const HG_NORMAL_BIAS: f32 = 1e-3;
const HG_LIMIT_EMPTY_SLOTS: u32 = 2u;
const HG_BUCKET_SIZE: u32 = 16u;
const HG_INVALID_INDEX: u32 = 0xffffffffu;

const SHARC_GRID_LOGARITHM_BASE: f32 = 2.0;
const SHARC_GRID_LEVEL_BIAS: f32 = 0.0;
const SHARC_ACCUMULATED_FRAME_NUM_MIN: u32 = 1u;
const SHARC_ACCUMULATED_FRAME_NUM_MAX: u32 = 1024u;
const SHARC_STALE_FRAME_NUM_MIN: u32 = 8u;
const SHARC_STALE_FRAME_NUM_MAX: u32 = 1024u;
const SHARC_SAMPLE_NUM_THRESHOLD: f32 = 0.0;
const SHARC_LINEAR_PROBE_WINDOW_SIZE: u32 = 8u;
const SHARC_PROPAGATION_DEPTH: u32 = 2u;
const SHARC_RESAMPLING_DEPTH_MIN: u32 = 1u;

@group(0) @binding(23) var<storage, read_write> sh_keys: array<atomic<u32>>; // 2 mots par entrée (lo, hi)
@group(0) @binding(24) var<storage, read_write> sh_lock: array<atomic<u32>>;
@group(0) @binding(25) var<storage, read_write> sh_acc: array<atomic<u32>>;  // SharcAccumulationData : uint4
@group(0) @binding(26) var<storage, read_write> sh_res: array<vec4<u32>>;    // SharcPackedData : half4, sampleData, ext

// --- clés 64 bits sur deux u32 ---
fn hg_put(k: vec2<u32>, v: u32, shift: u32) -> vec2<u32> {
    var r = k;
    if (shift >= 32u) {
        r.y |= v << (shift - 32u);
    } else {
        r.x |= v << shift;
        if (shift > 0u) { r.y |= v >> (32u - shift); }
    }
    return r;
}

fn hg_get(k: vec2<u32>, shift: u32, mask: u32) -> u32 {
    if (shift >= 32u) { return (k.y >> (shift - 32u)) & mask; }
    if (shift == 0u) { return k.x & mask; }
    return ((k.x >> shift) | (k.y << (32u - shift))) & mask;
}

fn hg_eq(a: vec2<u32>, b: vec2<u32>) -> bool { return a.x == b.x && a.y == b.y; }
fn hg_is_invalid(k: vec2<u32>) -> bool { return k.x == 0u && k.y == 0u; }

// --- HashGridCommon.h ---
fn hg_log_base(x: f32, base: f32) -> f32 { return log2(x) / log2(base); }

fn hg_jenkins(a0: u32) -> u32 {
    var a = a0;
    a = (a + 0x7ed55d16u) + (a << 12u);
    a = (a ^ 0xc761c23cu) ^ (a >> 19u);
    a = (a + 0x165667b1u) + (a << 5u);
    a = (a + 0xd3a2646cu) ^ (a << 9u);
    a = (a + 0xfd7046c5u) + (a << 3u);
    a = (a ^ 0xb55a4f09u) ^ (a >> 16u);
    return a;
}

fn hg_hash32(k: vec2<u32>) -> u32 { return hg_jenkins(k.x) ^ hg_jenkins(k.y); }

fn hg_base_slot(k: vec2<u32>) -> u32 {
    let base_count = fr.sh_cap - HG_BUCKET_SIZE + 1u;
    return hg_hash32(k) % base_count;
}

fn hg_level(p: vec3<f32>) -> u32 {
    let off = fr.eye - p;
    let d2 = max(dot(off, off), 1e-10);
    let level = 0.5 * hg_log_base(d2, SHARC_GRID_LOGARITHM_BASE) + SHARC_GRID_LEVEL_BIAS;
    return u32(clamp(level, 1.0, f32(HG_LEVEL_BIT_MASK)));
}

fn hg_voxel_size(level: u32) -> f32 {
    let e = log2(SHARC_GRID_LOGARITHM_BASE) * (f32(level) - SHARC_GRID_LEVEL_BIAS);
    return exp2(e) / fr.sh_scene_scale;
}

fn hg_key_from_grid(g: vec4<u32>, n: vec3<f32>) -> vec2<u32> {
    var k = vec2<u32>(0u);
    k = hg_put(k, g.x & HG_POSITION_BIT_MASK, 0u);
    k = hg_put(k, g.y & HG_POSITION_BIT_MASK, HG_POSITION_BIT_NUM);
    k = hg_put(k, g.z & HG_POSITION_BIT_MASK, 2u * HG_POSITION_BIT_NUM);
    k = hg_put(k, g.w & HG_LEVEL_BIT_MASK, HG_LEVEL_BIT_OFFSET);
    let nb = select(1u, 0u, n.x + HG_NORMAL_BIAS >= 0.0) + select(2u, 0u, n.y + HG_NORMAL_BIAS >= 0.0)
        + select(4u, 0u, n.z + HG_NORMAL_BIAS >= 0.0);
    return hg_put(k, nb, HG_NORMAL_BIT_OFFSET);
}

fn hg_key(p0: vec3<f32>, n: vec3<f32>) -> vec2<u32> {
    let p = p0 + vec3<f32>(HG_POSITION_BIAS);
    let level = hg_level(p);
    let vs = hg_voxel_size(level);
    let gp = vec3<i32>(floor(p / vs));
    return hg_key_from_grid(vec4<u32>(bitcast<vec3<u32>>(gp), level), n);
}

fn hg_load_key(slot: u32) -> vec2<u32> {
    return vec2<u32>(atomicLoad(&sh_keys[2u * slot]), atomicLoad(&sh_keys[2u * slot + 1u]));
}

// HashGridAtomicCompareExchange, variante !HASH_GRID_ENABLE_64_BIT_ATOMICS (tampon de verrous)
fn hg_cas(slot: u32, cmp: vec2<u32>, val: vec2<u32>) -> vec2<u32> {
    let lock = 0xaaaaaaaau;
    var original = vec2<u32>(0xffffffffu); // indéfini en HLSL si le verrou n'est jamais pris
    var fuse = 0u;
    var busy = true;
    while (busy && fuse < 8u) {
        let state = atomicExchange(&sh_lock[slot], lock);
        busy = state != 0u;
        if (state != lock) {
            original = hg_load_key(slot);
            if (hg_eq(original, cmp)) {
                atomicStore(&sh_keys[2u * slot], val.x);
                atomicStore(&sh_keys[2u * slot + 1u], val.y);
            }
            atomicExchange(&sh_lock[slot], state);
            fuse = 8u;
        }
        fuse++;
    }
    return original;
}

fn hg_insert(key: vec2<u32>, base: u32, probe_range: u32) -> u32 {
    let range = min(probe_range, HG_BUCKET_SIZE);
    for (var off = 0u; off < range; off++) {
        let prev = hg_cas(base + off, vec2<u32>(0u), key);
        if (hg_is_invalid(prev) || hg_eq(prev, key)) { return base + off; }
    }
    return HG_INVALID_INDEX;
}

fn hg_find(key: vec2<u32>, base: u32, probe_range: u32) -> u32 {
    let range = min(probe_range, HG_BUCKET_SIZE);
    var empty = 0u;
    for (var off = 0u; off < range; off++) {
        let stored = hg_load_key(base + off);
        if (hg_is_invalid(stored)) {
            if (empty > HG_LIMIT_EMPTY_SLOTS) { break; }
            empty++;
        }
        if (hg_eq(stored, key)) { return base + off; }
    }
    return HG_INVALID_INDEX;
}

// --- SharcCommon.h ---
struct ShVoxel { rad: vec3<f32>, n: f32, acc_frames: u32, stale: u32, ext: u32 };

fn sharc_unpack(r: vec4<u32>) -> ShVoxel {
    var v: ShVoxel;
    let a = unpack2x16float(r.x);
    let b = unpack2x16float(r.y);
    v.rad = vec3<f32>(a.x, a.y, b.x);
    v.n = b.y;
    v.acc_frames = r.z & 0xffffu;
    v.stale = (r.z >> 16u) & 0xffffu;
    v.ext = r.w;
    return v;
}

fn sharc_pack(rad: vec3<f32>, n: f32, acc_frames: u32, stale: u32, ext: u32) -> vec4<u32> {
    let m = min(vec4<f32>(rad, n), vec4<f32>(65504.0));
    return vec4<u32>(pack2x16float(m.xy), pack2x16float(m.zw), acc_frames | (stale << 16u), ext);
}

fn sharc_add_voxel(i: u32, value: vec3<f32>, weight: vec3<f32>, sample_data: u32) {
    let s = vec3<u32>(value * weight * fr.sh_rscale);
    if (s.x != 0u) { atomicAdd(&sh_acc[4u * i], s.x); }
    if (s.y != 0u) { atomicAdd(&sh_acc[4u * i + 1u], s.y); }
    if (s.z != 0u) { atomicAdd(&sh_acc[4u * i + 2u], s.z); }
    if (sample_data != 0u) { atomicAdd(&sh_acc[4u * i + 3u], sample_data); }
}

// SharcState (SHARC_UPDATE)
var<private> sh_idx: array<u32, 2>;
var<private> sh_w: array<vec3<f32>, 2>;
var<private> sh_len: u32;

fn sharc_update_miss(radiance: vec3<f32>) {
    for (var i = 0u; i < sh_len; i++) {
        sharc_add_voxel(sh_idx[i], radiance, sh_w[i], 0u);
    }
}

fn sharc_update_hit(p: vec3<f32>, n: vec3<f32>, direct: vec3<f32>, random: f32, demod: vec3<f32>) -> bool {
    var cont = true;
    let key = hg_key(p, n);
    let idx = hg_insert(key, hg_base_slot(key), HG_BUCKET_SIZE);
    if (idx == HG_INVALID_INDEX) { return false; }
    var rad = direct;
    let inv_demod = 1.0 / demod;
    // SHARC_ENABLE_CACHE_RESAMPLING
    let depth = u32(round(mix(f32(SHARC_RESAMPLING_DEPTH_MIN), f32(SHARC_PROPAGATION_DEPTH), random)));
    if (depth <= sh_len) {
        let v = sharc_unpack(sh_res[idx]);
        if (v.n > SHARC_SAMPLE_NUM_THRESHOLD) {
            rad = v.rad * demod;
            cont = false;
        }
    }
    if (cont) { sharc_add_voxel(idx, direct * inv_demod, vec3<f32>(1.0), 1u); }
    for (var i = 0u; i < sh_len; i++) {
        if (sh_idx[i] == HG_INVALID_INDEX) { continue; }
        sharc_add_voxel(sh_idx[i], rad, sh_w[i], 0u);
    }
    for (var i = min(sh_len, SHARC_PROPAGATION_DEPTH - 1u); i > 0u; i--) {
        sh_idx[i] = sh_idx[i - 1u];
        sh_w[i] = sh_w[i - 1u];
    }
    sh_idx[0] = idx;
    sh_w[0] = inv_demod;
    sh_len = min(sh_len + 1u, SHARC_PROPAGATION_DEPTH);
    return cont;
}

fn sharc_set_throughput(t: vec3<f32>) {
    for (var i = 0u; i < sh_len; i++) { sh_w[i] *= t; }
}

struct ShQuery { found: bool, rad: vec3<f32> };

fn sharc_get_cached(p: vec3<f32>, n: vec3<f32>, demod: vec3<f32>) -> ShQuery {
    var q: ShQuery;
    q.found = false;
    let key = hg_key(p, n);
    let idx = hg_find(key, hg_base_slot(key), HG_BUCKET_SIZE);
    if (idx == HG_INVALID_INDEX) { return q; }
    let v = sharc_unpack(sh_res[idx]);
    if (v.n > SHARC_SAMPLE_NUM_THRESHOLD) {
        q.found = true;
        q.rad = v.rad * demod;
    }
    return q;
}

fn sharc_adjacent_key(k: vec2<u32>) -> vec2<u32> {
    var gp = vec3<i32>(
        i32(hg_get(k, 0u, HG_POSITION_BIT_MASK)),
        i32(hg_get(k, HG_POSITION_BIT_NUM, HG_POSITION_BIT_MASK)),
        i32(hg_get(k, 2u * HG_POSITION_BIT_NUM, HG_POSITION_BIT_MASK)));
    gp = (gp << vec3<u32>(32u - HG_POSITION_BIT_NUM)) >> vec3<u32>(32u - HG_POSITION_BIT_NUM);
    var level = i32(hg_get(k, HG_LEVEL_BIT_OFFSET, HG_LEVEL_BIT_MASK));
    let inv = 1.0 / hg_voxel_size(u32(level));
    let cv = vec3<i32>(floor(fr.eye * inv)) - gp;
    let cvp = vec3<i32>(floor(fr.sh_cam_prev * inv)) - gp;
    if (dot(cv, cv) < dot(cvp, cvp)) {
        gp = vec3<i32>(floor(vec3<f32>(gp) / SHARC_GRID_LOGARITHM_BASE));
        level = min(level + 1, i32(HG_LEVEL_BIT_MASK));
    } else {
        gp = vec3<i32>(floor(vec3<f32>(gp) * SHARC_GRID_LOGARITHM_BASE));
        level = max(level - 1, 1);
    }
    var r = vec2<u32>(0u);
    r = hg_put(r, bitcast<u32>(gp.x) & HG_POSITION_BIT_MASK, 0u);
    r = hg_put(r, bitcast<u32>(gp.y) & HG_POSITION_BIT_MASK, HG_POSITION_BIT_NUM);
    r = hg_put(r, bitcast<u32>(gp.z) & HG_POSITION_BIT_MASK, 2u * HG_POSITION_BIT_NUM);
    r = hg_put(r, u32(level) & HG_LEVEL_BIT_MASK, HG_LEVEL_BIT_OFFSET);
    return hg_put(r, hg_get(k, HG_NORMAL_BIT_OFFSET, HG_NORMAL_BIT_MASK), HG_NORMAL_BIT_OFFSET);
}

// SharcResolveEntry : un fil par entrée de la table
@compute @workgroup_size(256)
fn sharc_resolve(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {
    let e = gid.x + gid.y * nwg.x * 256u;
    if (e >= fr.sh_cap) { return; }
    let key = hg_load_key(e);
    if (hg_is_invalid(key)) { return; }
    let acc4 = vec4<u32>(atomicLoad(&sh_acc[4u * e]), atomicLoad(&sh_acc[4u * e + 1u]),
        atomicLoad(&sh_acc[4u * e + 2u]), atomicLoad(&sh_acc[4u * e + 3u]));
    var resolved = sh_res[e];
    var v = sharc_unpack(resolved);
    let sample_num = f32(acc4.w);
    var sample_num_prev = v.n;
    var acc_frames = v.acc_frames + 1u;
    var stale = select(v.stale + 1u, 0u, sample_num != 0.0);
    let stale_max = clamp(fr.sh_stale, SHARC_STALE_FRAME_NUM_MIN, SHARC_STALE_FRAME_NUM_MAX);
    if (stale >= stale_max) {
        atomicStore(&sh_keys[2u * e], 0u);
        atomicStore(&sh_keys[2u * e + 1u], 0u);
        for (var w = 0u; w < 4u; w++) { atomicStore(&sh_acc[4u * e + w], 0u); }
        sh_res[e] = vec4<u32>(0u);
        return;
    } else if (sample_num == 0.0) {
        sh_res[e].z = resolved.z + (1u | (1u << 16u));
        return;
    }
    // entrée déjà présente plus loin (collisions lors d'insertions précédentes)
    if (sample_num_prev == 0.0) {
        let end = min(e + 1u + SHARC_LINEAR_PROBE_WINDOW_SIZE, fr.sh_cap);
        for (var i = e + 1u; i < end; i++) {
            if (hg_eq(hg_load_key(i), key)) {
                resolved = sh_res[i];
                v = sharc_unpack(resolved);
                sample_num_prev = v.n;
                acc_frames = v.acc_frames + 1u;
                stale = 0u;
                break;
            }
        }
    }
    var rad = vec3<f32>(acc4.xyz) / (fr.sh_rscale * max(sample_num, 1e-6));
    let rad_prev = v.rad;
    let acc_max = clamp(fr.sh_accum, SHARC_ACCUMULATED_FRAME_NUM_MIN, SHARC_ACCUMULATED_FRAME_NUM_MAX);
    if (acc_frames > acc_max) {
        sample_num_prev *= f32(acc_max) / f32(acc_frames);
        acc_frames = acc_max;
    }
    let inv_total = 1.0 / (sample_num_prev + sample_num);
    rad = rad_prev * (sample_num_prev * inv_total) + rad * (sample_num * inv_total);
    var total = sample_num_prev + sample_num;
    // SHARC_BLEND_ADJACENT_LEVELS : reprise du niveau voisin quand la caméra bouge
    let co = fr.eye - fr.sh_cam_prev;
    if (dot(co, co) > 1e-6 && acc_frames <= 2u) {
        let ak = sharc_adjacent_key(key);
        let ai = hg_find(ak, hg_base_slot(ak), HG_BUCKET_SIZE);
        if (ai != HG_INVALID_INDEX) {
            let av = sharc_unpack(sh_res[ai]);
            if (av.n > SHARC_SAMPLE_NUM_THRESHOLD) {
                let bw = 1.0 / (av.n + total);
                rad = av.rad * (av.n * bw) + rad * (total * bw);
                total += av.n;
            }
        }
    }
    sh_res[e] = sharc_pack(rad, total, acc_frames, stale, v.ext);
    for (var w = 0u; w < 4u; w++) { atomicStore(&sh_acc[4u * e + w], 0u); }
}

// --- intégration (équivalent de Pathtracer.hlsl de l'exemple) ---

// éclairage direct d'un sommet lambertien (nee_split, krc_gpu.wgsl)
fn sharc_nee(p: vec3<f32>, n: vec3<f32>, albedo: vec3<f32>) -> vec3<f32> {
    return nee_split(p, n, n, albedo, 1.0, 0.0).d;
}

// SHARC_MATERIAL_DEMODULATION de l'exemple : max(albédo diffus, 0,05) + max(F0, 0,02) × luminance
// de l'albédo spéculaire moyen (lambertien pur : max(albédo, 0,05))
fn sharc_demod(albedo: vec3<f32>) -> vec3<f32> { return max(albedo, vec3<f32>(0.05)); }
fn sharc_demod2(base: vec3<f32>, rough: f32, metal: f32) -> vec3<f32> {
    if (is_diffuse(rough, metal)) { return sharc_demod(base); }
    let f0 = f0_of(base, metal);
    let favg = f0 + (vec3<f32>(1.0) - f0) / 21.0;
    return max(diff_albedo(base, metal), vec3<f32>(0.05)) + max(f0, vec3<f32>(0.02)) * lum(favg);
}

// Passe Update : un pixel tiré au hasard par bloc sh_down × sh_down, chemin dont chaque segment
// alimente le cache (débit remis à 1 à chaque rebond, propagation par SharcSetThroughput)
@compute @workgroup_size(64)
fn sharc_update(@builtin(global_invocation_id) gid: vec3<u32>) {
    let bw = (fr.w + fr.sh_down - 1u) / fr.sh_down;
    let bh = (fr.h + fr.sh_down - 1u) / fr.sh_down;
    let b = gid.x;
    if (b >= bw * bh) { return; }
    rec_on = false;
    uni = false;
    split = false;
    sel = 0u;
    seed_rng(b, 0x5a4cu, fr.frame);
    let x = (b % bw) * fr.sh_down + u32(rnd() * f32(fr.sh_down));
    let y = (b / bw) * fr.sh_down + u32(rnd() * f32(fr.sh_down));
    if (x >= fr.w || y >= fr.h) { return; }
    let g = gbuf[y * fr.w + x];
    if (g.kind != 2u) { return; }
    atomicAdd(&ctr[C_TASKS], 1u);
    sh_len = 0u;
    var p = g.p;
    var n = g.n;
    var albedo = g.albedo;
    var rough = g_rough(g);
    var metal = g_metal(g);
    var wo = normalize(fr.eye - g.p);
    for (var depth = 0u; depth < MAX_DEPTH; depth++) {
        let ns = nee_split(p, n, wo, albedo, rough, metal);
        if (!sharc_update_hit(p, n, ns.d + ns.s, rnd(), sharc_demod2(albedo, rough, metal))) { break; }
        var d: vec3<f32>;
        if (is_diffuse(rough, metal)) {
            d = cosine_dir(n);
            sharc_set_throughput(albedo); // poids BRDF lambertien, échantillonnage en cosinus
        } else {
            let bs = bsdf_sample(n, wo, albedo, rough, metal, false);
            if (!bs.ok) { break; }
            d = bs.wi;
            sharc_set_throughput(bs.w);
        }
        let h = intersect(p + n * OFFSET, d);
        if (!h.found || h.light >= 0) {
            sharc_update_miss(vec3<f32>(0.0)); // ciel noir ; lampes comptées par l'éclairage direct
            break;
        }
        p = h.p;
        n = h.n;
        albedo = h.albedo;
        rough = h.rough;
        metal = h.metal;
        wo = -d;
    }
}

// Vue du cache à la caméra (mode debug de l'exemple : SharcGetCachedRadiance au premier impact)
@compute @workgroup_size(64)
fn sharc_query(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    var v = vec3<f32>(0.0);
    if (g.kind == 1u) {
        v = g.albedo;
    } else if (g.kind == 2u) {
        let q = sharc_get_cached(g.p, g.n, sharc_demod2(g.albedo, g_rough(g), g_metal(g)));
        if (q.found) { v = q.rad; }
    }
    image[k] = vec4<f32>(v, f32(g.kind));
}

// Passe Render de l'exemple : un chemin par pixel, cache interrogé aux impacts non primaires
// assez loin (segment > taille de voxel × √3, empreinte du lobe > voxel), puis arrêt
@compute @workgroup_size(64)
fn sharc_render(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    var v = vec3<f32>(0.0);
    var vs_spec = vec3<f32>(0.0); // mode jeu : partie spéculaire (premier lobe spéculaire)
    if (g.kind == 1u) {
        v = g.albedo;
    } else if (g.kind == 2u) {
        rec_on = false;
        uni = false;
        split = false;
        sel = 0u;
        seed_rng(k, 0x7e4du, fr.frame);
        var p = g.p;
        var n = g.n;
        var albedo = g.albedo;
        var rough = g_rough(g);
        var metal = g_metal(g);
        var wo = normalize(fr.eye - g.p);
        var beta = vec3<f32>(1.0);
        var seg = 0.0;
        var rough_prev = 0.0;
        var to_spec = false;
        for (var depth = 0u; depth < MAX_DEPTH; depth++) {
            let vs = hg_voxel_size(hg_level(p));
            var valid = seg > vs * sqrt(3.0);
            let rp = min(rough_prev, 0.99);
            let alpha = rp * rp;
            valid = valid && seg * sqrt(0.5 * alpha * alpha / (1.0 - alpha * alpha)) > vs;
            if (valid) {
                let q = sharc_get_cached(p, n, sharc_demod2(albedo, rough, metal));
                if (q.found) {
                    if (to_spec) { vs_spec += beta * q.rad; } else { v += beta * q.rad; }
                    break;
                }
            }
            let ns = nee_split(p, n, wo, albedo, rough, metal);
            if (depth == 0u) {
                v += ns.d;
                vs_spec += ns.s;
            } else if (to_spec) {
                vs_spec += beta * (ns.d + ns.s);
            } else {
                v += beta * (ns.d + ns.s);
            }
            var d: vec3<f32>;
            if (is_diffuse(rough, metal)) {
                rough_prev += 1.0; // lobe diffus
                d = cosine_dir(n);
                beta = beta * albedo;
            } else {
                let bs = bsdf_sample(n, wo, albedo, rough, metal, false);
                if (!bs.ok) { break; }
                rough_prev += bs.lobe_rough;
                if (depth == 0u) { to_spec = bs.spec; }
                d = bs.wi;
                beta = beta * bs.w;
            }
            let h = intersect(p + n * OFFSET, d);
            if (!h.found || h.light >= 0) { break; }
            seg = h.t;
            p = h.p;
            n = h.n;
            albedo = h.albedo;
            rough = h.rough;
            metal = h.metal;
            wo = -d;
            if (depth >= 2u) {
                let q = clamp(max(beta.x, max(beta.y, beta.z)), 0.05, 0.95);
                if (rnd() > q) { break; }
                beta = beta / q;
            }
        }
    }
    if ((fr.flags & F_GAME) != 0u) {
        image[k] = vec4<f32>(v, f32(g.kind));
        spec_img[k] = vec4<f32>(vs_spec, 0.0);
    } else {
        image[k] = vec4<f32>(v + vs_spec, f32(g.kind));
    }
}

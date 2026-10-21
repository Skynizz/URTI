// KRC entièrement sur GPU : un pipeline de passes compute par frame.
// Mêmes règles que methods.rs (krc_energy, dépendances L2), avec deux simplifications
// documentées : allocation de Neyman par bisection du seuil de gain (équivalent continu du
// glouton) et un pixel de référence par cellule et par frame (tiré uniformément).

const REGION: f32 = 0.6;
const ORIGIN: vec3<f32> = vec3<f32>(-0.2, -0.2, -0.2);
const DIMS: vec3<i32> = vec3<i32>(72, 6, 8);
const SUPER_X: i32 = 18;
const MAX_DEPTH: u32 = 8u;
const MISS_LEN: f32 = 30.0;
const EPS: f32 = 1e-4;
const OFFSET: f32 = 1e-4;
const PI: f32 = 3.14159265358979;
const LIGHT_ID_BASE: u32 = 64u;

const K: u32 = 4u;
const M_DELTA0: u32 = 2u;
const M_DELTA: u32 = 8u;
const SAFETY_WINDOW: f32 = 32.0;
const Z_SAFETY: f32 = 4.0;
const EXPLORE_PERIOD: u32 = 16u;
const KAPPA: f32 = 4.0;
const PRIOR_FLOOR: f32 = 0.1;
const REL_EPS: f32 = 0.01;
const DEP_WINDOW: u32 = 64u;
const EPS_ENERGY: f32 = 0.01;
const DEBT_RATE: f32 = 0.5;
const CELL_PX: f32 = 4.0;
const EMA_MIN: f32 = 0.015625;
const MAX_PER_CELL: u32 = 64u;
const N_POST: f32 = 64.0;  // poids max d'une estimation corrigée (sa variance vient de 8 tirages) ; réglé par le test de dérive

// compteurs (ctr)
const C_CELLS: u32 = 0u;
const C_TASKS: u32 = 1u;     // curseur d'écriture dans `tasks`
const C_SURF: u32 = 2u;
const C_SUSPECTS: u32 = 3u;
const C_DPATHS: u32 = 4u;
const C_DOLD: u32 = 5u;
const C_DZERO: u32 = 6u;
const C_HARD: u32 = 7u;
const C_SAFETY: u32 = 8u;
const C_DEBT: u32 = 9u;
const C_D1: u32 = 10u;       // tâches delta du 1er tour : [0, C_D1)
const C_D2: u32 = 11u;       // fin du 2e tour
const C_REG0: u32 = 12u;     // début des tâches ordinaires
const C_VISIBLE: u32 = 13u;
const C_MATURE: u32 = 14u;
const C_OVERFLOW: u32 = 15u;
const C_EXPLORE: u32 = 16u;
const C_MAXN: u32 = 17u;     // max des n des cellules (bits f32 positifs : ordre préservé)
const C_HIST: u32 = 32u;     // 64 cases
const C_SCAN: u32 = 96u;     // 3 × 32 sommes de quotas candidates (trois balayages)

// accumulateurs atomiques par cellule (acc), en mots
const ACC: u32 = 44u;
const A_FOOT: u32 = 0u;
const A_REF: u32 = 1u;
const A_FN: u32 = 2u;
const A_FR: u32 = 3u;        // 3, 4, 5 : Σ rgb
const A_FL: u32 = 6u;
const A_FL2: u32 = 7u;
const A_DEPS: u32 = 8u;      // 8, 9 : super-régions traversées (fenêtre courante)
const A_NS: u32 = 10u;       // échantillons dans la fenêtre courante
const A_FREQ: u32 = 11u;     // 11..28 : 36 compteurs u16 de sommets par super-région
const A_PM: u32 = 29u;
const A_PDIFF: u32 = 30u;
const A_PAFF: u32 = 31u;
const A_PX: u32 = 32u;       // 32, 33, 34 : Σx rgb
const A_PY: u32 = 35u;       // 35, 36, 37 : Σy rgb
const A_PXL: u32 = 38u;
const A_PYL: u32 = 39u;
const A_PXL2: u32 = 40u;
const A_PYL2: u32 = 41u;
const A_PXYL: u32 = 42u;
const A_SUSP: u32 = 43u;

// fr.flags
const F_EVENT: u32 = 1u;
const F_GEOMETRY: u32 = 2u;
const F_SHARC: u32 = 4u;

struct Frame {
    w: u32, h: u32, n_boxes: u32, n_lights: u32,
    frame: u32, seed: u32, flags: u32, table_mask: u32,
    cell_cap: u32, round: u32, ev_lights: u32, pad0: u32,
    eye: vec3<f32>, px_angle: f32,
    fwd: vec3<f32>, th: f32,
    right: vec3<f32>, tv: f32,
    up: vec3<f32>, pad1: f32,
};
struct Box { mn: vec3<f32>, p0: f32, mx: vec3<f32>, p1: f32, albedo: vec3<f32>, p2: f32 };
struct Light { center: vec3<f32>, hx: f32, hz: f32, on: f32, power: f32, toggle_w: f32, emission: vec3<f32>, p0: f32 };
struct Gpx { p: vec3<f32>, kind: u32, n: vec3<f32>, dist: f32, albedo: vec3<f32>, p0: u32 };
struct Cell {
    n: f32, lmean: f32, m2: f32, last: u32,
    mean: vec3<f32>, rn: f32,
    rl: f32, rm2: f32, ns_prev: u32, p0: u32,
    deps_prev: vec2<u32>, cx: f32, p1: u32,
    freq_prev: array<u32, 18>,
};

@group(0) @binding(0) var<uniform> fr: Frame;
@group(0) @binding(1) var<storage, read> boxes: array<Box>;
@group(0) @binding(2) var<storage, read> lights: array<Light>;
@group(0) @binding(3) var<storage, read> boxes_prev: array<Box>;
@group(0) @binding(4) var<storage, read> lights_prev: array<Light>;
@group(0) @binding(5) var<storage, read_write> gbuf: array<Gpx>;
@group(0) @binding(6) var<storage, read_write> pix_cells: array<u32>;
@group(0) @binding(7) var<storage, read_write> keys: array<atomic<u32>>;
@group(0) @binding(8) var<storage, read_write> vals: array<atomic<u32>>;
@group(0) @binding(9) var<storage, read_write> ctr: array<atomic<u32>>;
@group(0) @binding(10) var<storage, read_write> cells: array<Cell>;
@group(0) @binding(11) var<storage, read_write> acc: array<atomic<u32>>;
@group(0) @binding(12) var<storage, read_write> tasks: array<vec4<u32>>;
@group(0) @binding(13) var<storage, read_write> image: array<vec4<f32>>;
// événement : [0,108) régions fines, [108,110) super-régions, [110,146) magnitude par
// super-région (f32), [146,162) poids des lampes (f32)
@group(0) @binding(14) var<storage, read> ev: array<u32>;
@group(0) @binding(15) var<storage, read_write> scal: array<f32>; // 0 : λ, 1 : a priori, 2 : budget, 3-4 : bornes log2 λ
@group(0) @binding(16) var<storage, read_write> ind: array<u32>; // arguments de dispatch indirect
// résumé compact par cellule pour l'allocation : (empreinte, n, variance relative, tirage)
@group(0) @binding(17) var<storage, read_write> qbuf: array<vec4<f32>>;

// ---------------------------------------------------------------- outils
var<private> rng: u32;
var<private> sel: u32;        // 0 : scène courante, 1 : scène précédente
var<private> rec_on: bool;
var<private> uni: bool;
var<private> split: bool;
var<private> m_trav: vec2<u32>;
var<private> m_vsup: vec2<u32>;
var<private> m_lights: u32;
var<private> m_aff: bool;     // le chemin touche-t-il une région de l'événement ?

fn pcg(v: u32) -> u32 {
    let s = v * 747796405u + 2891336453u;
    let w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

fn rnd() -> f32 {
    rng = rng * 747796405u + 2891336453u;
    let s = rng;
    var w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    w = (w >> 22u) ^ w;
    return f32(w >> 8u) * (1.0 / 16777216.0);
}

fn seed_rng(a: u32, b: u32, c: u32) {
    rng = pcg(a ^ pcg(b ^ pcg(c + fr.seed)));
}

fn add_f32(i: u32, v: f32) {
    var old = atomicLoad(&acc[i]);
    loop {
        let r = atomicCompareExchangeWeak(&acc[i], old, bitcast<u32>(bitcast<f32>(old) + v));
        if (r.exchanged) { break; }
        old = r.old_value;
    }
}

fn ld_f32(i: u32) -> f32 {
    return bitcast<f32>(atomicLoad(&acc[i]));
}

fn n_boxes() -> u32 { return fr.n_boxes; }
fn n_lights() -> u32 { return fr.n_lights; }
fn box_at(i: u32) -> Box { if (sel == 0u) { return boxes[i]; } return boxes_prev[i]; }
fn light_at(j: u32) -> Light { if (sel == 0u) { return lights[j]; } return lights_prev[j]; }

// ---------------------------------------------------------------- régions
fn cell_of(p: vec3<f32>) -> vec3<i32> {
    return clamp(vec3<i32>(floor((p - ORIGIN) / REGION)), vec3<i32>(0), DIMS - vec3<i32>(1));
}
fn index_of(c: vec3<i32>) -> u32 { return u32((c.z * DIMS.y + c.y) * DIMS.x + c.x); }
fn super_of(c: vec3<i32>) -> u32 { return u32((c.z / 4) * SUPER_X + c.x / 4); }
fn set2(m: vec2<u32>, s: u32) -> vec2<u32> {
    if (s < 32u) { return vec2<u32>(m.x | (1u << s), m.y); }
    return vec2<u32>(m.x, m.y | (1u << (s - 32u)));
}

fn mark_vertex(p: vec3<f32>) {
    if (rec_on) { m_vsup = set2(m_vsup, super_of(cell_of(p))); }
}

// Grille des super-régions (dépendances stockées) : 18 × 1 × 2 blocs de 2,4 × 3,6 × 2,4 m.
const SUP: vec3<f32> = vec3<f32>(2.4, 3.6, 2.4);
const SDIMS: vec3<i32> = vec3<i32>(18, 1, 2);
var<private> fine_dda: bool;

// DDA : super-régions traversées (dépendances L2). Sur la grille fine (0,6 m) seulement quand il faut
// aussi le test exact de contact avec l'événement (chemins corrélés) ; sinon sur la grille grossière,
// ~4× moins de pas.
fn mark_segment(a: vec3<f32>, b: vec3<f32>) {
    if (!rec_on) { return; }
    let d = b - a;
    let cs = select(SUP, vec3<f32>(REGION), fine_dda);
    let dims = select(SDIMS, DIMS, fine_dda);
    let lo = ORIGIN;
    let hi = ORIGIN + vec3<f32>(dims) * cs;
    var t0 = 0.0;
    var t1 = 1.0;
    for (var ax = 0; ax < 3; ax++) {
        if (abs(d[ax]) < 1e-12) {
            if (a[ax] < lo[ax] || a[ax] > hi[ax]) { return; }
        } else {
            let ta = (lo[ax] - a[ax]) / d[ax];
            let tb = (hi[ax] - a[ax]) / d[ax];
            t0 = max(t0, min(ta, tb));
            t1 = min(t1, max(ta, tb));
        }
    }
    if (t0 > t1) { return; }
    let p = a + d * t0;
    var c = clamp(vec3<i32>(floor((p - ORIGIN) / cs)), vec3<i32>(0), dims - vec3<i32>(1));
    var stp = vec3<i32>(0);
    var tmax = vec3<f32>(1e30);
    var tdelta = vec3<f32>(1e30);
    for (var ax = 0; ax < 3; ax++) {
        if (abs(d[ax]) >= 1e-12) {
            let pos = d[ax] > 0.0;
            stp[ax] = select(-1, 1, pos);
            let boundary = ORIGIN[ax] + f32(c[ax] + select(0, 1, pos)) * cs[ax];
            tmax[ax] = t0 + (boundary - p[ax]) / d[ax];
            tdelta[ax] = cs[ax] / abs(d[ax]);
        }
    }
    let test_ev = fine_dda && (fr.flags & F_EVENT) != 0u;
    for (var it = 0; it < 256; it++) {
        m_trav = set2(m_trav, select(u32(c.z * SUPER_X + c.x), super_of(c), fine_dda));
        if (test_ev && !m_aff) {
            let idx = index_of(c);
            if ((ev[idx >> 5u] >> (idx & 31u) & 1u) == 1u) { m_aff = true; }
        }
        var ax = 2;
        if (tmax.x < tmax.y) {
            if (tmax.x < tmax.z) { ax = 0; }
        } else if (tmax.y < tmax.z) {
            ax = 1;
        }
        if (tmax[ax] > t1) { break; }
        c[ax] += stp[ax];
        if (c[ax] < 0 || c[ax] >= dims[ax]) { break; }
        tmax[ax] += tdelta[ax];
    }
}

// ---------------------------------------------------------------- géométrie
struct Hit { found: bool, t: f32, p: vec3<f32>, n: vec3<f32>, albedo: vec3<f32>, light: i32 };

fn safe_dir(d: vec3<f32>) -> vec3<f32> {
    let s = select(vec3<f32>(1.0), vec3<f32>(-1.0), d < vec3<f32>(0.0));
    return select(d, s * 1e-12, abs(d) < vec3<f32>(1e-12));
}

fn slab(mn: vec3<f32>, mx: vec3<f32>, o: vec3<f32>, inv: vec3<f32>) -> vec3<f32> {
    let ta = (mn - o) * inv;
    let tb = (mx - o) * inv;
    let tlo = min(ta, tb);
    let thi = max(ta, tb);
    var t0 = tlo.x;
    var axis = 0.0;
    if (tlo.y > t0) { t0 = tlo.y; axis = 1.0; }
    if (tlo.z > t0) { t0 = tlo.z; axis = 2.0; }
    return vec3<f32>(t0, min(thi.x, min(thi.y, thi.z)), axis);
}

fn intersect(o: vec3<f32>, d: vec3<f32>) -> Hit {
    var h: Hit;
    h.found = false;
    h.t = 1e30;
    h.light = -1;
    let inv = 1.0 / safe_dir(d);
    var axis = 0u;
    var id = 0u;
    for (var i = 0u; i < n_boxes(); i++) {
        let b = box_at(i);
        let s = slab(b.mn, b.mx, o, inv);
        if (s.x <= s.y && s.x > EPS && s.x < h.t) {
            h.t = s.x;
            id = i;
            axis = u32(s.z);
            h.found = true;
        }
    }
    for (var j = 0u; j < n_lights(); j++) {
        if (abs(d.y) < 1e-12) { continue; }
        let l = light_at(j);
        let t = (l.center.y - o.y) / d.y;
        if (t > EPS && t < h.t) {
            let p = o + d * t;
            if (abs(p.x - l.center.x) <= l.hx && abs(p.z - l.center.z) <= l.hz) {
                h.t = t;
                h.light = i32(j);
                h.found = true;
            }
        }
    }
    if (h.found) {
        h.p = o + d * h.t;
        if (h.light >= 0) {
            h.n = select(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, -1.0, 0.0), d.y > 0.0);
            h.albedo = vec3<f32>(0.0);
        } else {
            var nn = vec3<f32>(0.0);
            nn[axis] = select(1.0, -1.0, d[axis] > 0.0);
            h.n = nn;
            h.albedo = box_at(id).albedo;
        }
    }
    return h;
}

struct Occ { found: bool, t: f32 };

fn occluder(o: vec3<f32>, d: vec3<f32>, tmax: f32) -> Occ {
    var r: Occ;
    r.found = false;
    r.t = tmax;
    let inv = 1.0 / safe_dir(d);
    for (var i = 0u; i < n_boxes(); i++) {
        let b = box_at(i);
        let s = slab(b.mn, b.mx, o, inv);
        if (s.x <= s.y && s.x > EPS && s.x < r.t && s.y > 0.0) {
            r.found = true;
            r.t = s.x;
        }
    }
    return r;
}

fn cosine_dir(n: vec3<f32>) -> vec3<f32> {
    let a = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(n.x) > 0.9);
    let t = normalize(cross(n, a));
    let b = cross(n, t);
    let r = sqrt(rnd());
    let phi = 2.0 * PI * rnd();
    let z = sqrt(max(0.0, 1.0 - r * r));
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * z);
}

fn light_power(j: u32) -> f32 {
    let l = light_at(j);
    if (uni || l.on > 0.5) { return l.power; }
    return 0.0;
}

fn toggle_w(j: u32) -> f32 {
    if (!split) { return 0.0; }
    return bitcast<f32>(ev[146u + j]);
}

struct LL { l: vec3<f32>, dl: vec3<f32> };

fn radiance(p0: vec3<f32>, n0: vec3<f32>, a0: vec3<f32>) -> LL {
    var p = p0;
    var n = n0;
    var albedo = a0;
    var beta = vec3<f32>(1.0);
    var res: LL;
    res.l = vec3<f32>(0.0);
    res.dl = vec3<f32>(0.0);
    mark_vertex(p0);
    for (var depth = 0u; depth < MAX_DEPTH; depth++) {
        let origin = p + n * OFFSET;
        var total = 0.0;
        for (var j = 0u; j < n_lights(); j++) { total += light_power(j); }
        if (total > 0.0) {
            var u = rnd() * total;
            var s = n_lights();
            var pmf = 0.0;
            for (var j = 0u; j < n_lights(); j++) {
                let pw = light_power(j);
                if (pw > 0.0 && (u < pw || j == n_lights() - 1u)) {
                    s = j;
                    pmf = pw / total;
                    break;
                }
                u -= pw;
            }
            if (s < n_lights()) {
                if (rec_on) { m_lights = m_lights | (1u << s); }
                let li = light_at(s);
                let q = li.center + vec3<f32>((2.0 * rnd() - 1.0) * li.hx, 0.0, (2.0 * rnd() - 1.0) * li.hz);
                let w = q - origin;
                let dist2 = dot(w, w);
                let dist = sqrt(dist2);
                let wd = w / dist;
                let cos_x = dot(n, wd);
                let cos_l = wd.y;
                let on = li.on > 0.5;
                let tw = toggle_w(s);
                if (cos_x > 0.0 && cos_l > 0.0 && !on && tw == 0.0) {
                    mark_segment(origin, q);
                } else if (cos_x > 0.0 && cos_l > 0.0) {
                    let occ = occluder(origin, wd, dist - 1e-4);
                    if (occ.found) {
                        mark_segment(origin, origin + wd * occ.t);
                    } else {
                        mark_segment(origin, q);
                        let g = cos_x * cos_l / dist2 * (4.0 * li.hx * li.hz) / pmf / PI;
                        let c = beta * albedo * li.emission * g;
                        if (on) { res.l += c; }
                        res.dl += c * tw;
                    }
                }
            }
        }
        let d = cosine_dir(n);
        beta = beta * albedo;
        let h = intersect(origin, d);
        if (!h.found) {
            mark_segment(origin, origin + d * MISS_LEN);
            break;
        }
        mark_segment(origin, h.p);
        if (h.light >= 0) { break; }
        mark_vertex(h.p);
        p = h.p;
        n = h.n;
        albedo = h.albedo;
        if (depth >= 2u) {
            let q = clamp(max(beta.x, max(beta.y, beta.z)), 0.05, 0.95);
            if (rnd() > q) { break; }
            beta = beta / q;
        }
    }
    return res;
}

fn lum(c: vec3<f32>) -> f32 { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

// enregistre les dépendances d'un chemin dans la cellule
fn commit_deps(c: u32) {
    let b = c * ACC;
    if (m_trav.x != 0u) { atomicOr(&acc[b + A_DEPS], m_trav.x); }
    if (m_trav.y != 0u) { atomicOr(&acc[b + A_DEPS + 1u], m_trav.y); }
    atomicAdd(&acc[b + A_NS], 1u);
    // seulement les bits allumés (≈ 2 par chemin)
    for (var h = 0u; h < 2u; h++) {
        var m = m_vsup[h];
        while (m != 0u) {
            let s = h * 32u + countTrailingZeros(m);
            m &= m - 1u;
            atomicAdd(&acc[b + A_FREQ + (s >> 1u)], select(1u, 65536u, (s & 1u) == 1u));
        }
    }
}

// Entrelacement des tâches : les tâches d'une même cellule sont consécutives ; un warp qui les
// traiterait ensemble se battrait sur les mêmes atomiques. Le thread g prend la tâche
// (g mod 8)·S + g/8, avec S = ⌈N/8⌉.
fn stride_task(g: u32, n: u32) -> u32 {
    let s = (n + 7u) / 8u;
    return (g % 8u) * s + g / 8u;
}

fn reset_rec() {
    m_trav = vec2<u32>(0u);
    m_vsup = vec2<u32>(0u);
    m_lights = 0u;
    m_aff = false;
}

// ---------------------------------------------------------------- passes
@compute @workgroup_size(64)
fn gbuffer(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let i = k % fr.w;
    let j = k / fr.w;
    let sx = 2.0 * (f32(i) + 0.5) / f32(fr.w) - 1.0;
    let sy = 1.0 - 2.0 * (f32(j) + 0.5) / f32(fr.h);
    let d = normalize(fr.fwd + fr.right * (sx * fr.th) + fr.up * (sy * fr.tv));
    sel = 0u;
    let h = intersect(fr.eye, d);
    var g: Gpx;
    g.kind = 0u;
    if (h.found) {
        g.p = h.p;
        g.n = h.n;
        g.dist = h.t;
        if (h.light >= 0) {
            let l = lights[u32(h.light)];
            g.kind = 1u;
            g.albedo = select(vec3<f32>(0.0), l.emission, l.on > 0.5 && d.y > 0.0);
        } else {
            g.kind = 2u;
            g.albedo = h.albedo;
            atomicAdd(&ctr[C_SURF], 1u);
        }
    }
    gbuf[k] = g;
}

fn cell_key(k: u32, slot: u32) -> u32 {
    let g = gbuf[k];
    let level = floor(log2(CELL_PX * g.dist * fr.px_angle));
    let s = exp2(level);
    let h = pcg(k * 8u + slot + 0x5bd1e995u);
    let j = vec2<f32>(f32(h & 0xffffu) / 65536.0 - 0.5, f32((h >> 16u) & 0xffffu) / 65536.0 - 0.5);
    var axis = 2u;
    if (abs(g.n.x) > 0.5) { axis = 0u; } else if (abs(g.n.y) > 0.5) { axis = 1u; }
    var q: vec3<i32>;
    var ji = 0u;
    for (var a = 0u; a < 3u; a++) {
        let c = g.p[a] / s;
        if (a == axis) {
            q[a] = i32(round(c));
        } else {
            q[a] = i32(floor(c + j[ji]));
            ji++;
        }
    }
    let naxis = axis * 2u + select(0u, 1u, g.n[axis] > 0.0);
    var key = pcg(bitcast<u32>(i32(level)) ^ (naxis << 24u));
    key = pcg(key ^ bitcast<u32>(q.x));
    key = pcg(key ^ bitcast<u32>(q.y));
    key = pcg(key ^ bitcast<u32>(q.z));
    return max(key, 1u);
}

// 1er temps : réserver les clés nouvelles (sans attente active)
@compute @workgroup_size(256)
fn insert(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x / K;
    let slot = gid.x % K;
    if (k >= fr.w * fr.h || gbuf[k].kind != 2u) { return; }
    let key = cell_key(k, slot);
    var h = key & fr.table_mask;
    for (var probe = 0u; probe < 64u; probe++) {
        let r = atomicCompareExchangeWeak(&keys[h], 0u, key);
        if (r.exchanged) {
            let c = atomicAdd(&ctr[C_CELLS], 1u);
            if (c >= fr.cell_cap) {
                atomicAdd(&ctr[C_OVERFLOW], 1u);
                return;
            }
            cells[c].cx = gbuf[k].p.x;
            atomicStore(&vals[h], c + 1u);
            return;
        }
        if (r.old_value == key) { return; }
        if (r.old_value == 0u) { probe--; continue; } // échec fallacieux de CAS faible
        h = (h + 1u) & fr.table_mask;
    }
    atomicAdd(&ctr[C_OVERFLOW], 1u);
}

// 2e temps : retrouver la cellule, empreinte, pixel de référence tiré uniformément
@compute @workgroup_size(256)
fn lookup(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x / K;
    let slot = gid.x % K;
    if (k >= fr.w * fr.h) { return; }
    if (gbuf[k].kind != 2u) {
        pix_cells[gid.x] = 0xffffffffu;
        return;
    }
    let key = cell_key(k, slot);
    var h = key & fr.table_mask;
    var c = 0xffffffffu;
    for (var probe = 0u; probe < 64u; probe++) {
        let kk = atomicLoad(&keys[h]);
        if (kk == key) { c = atomicLoad(&vals[h]) - 1u; break; }
        if (kk == 0u) { break; }
        h = (h + 1u) & fr.table_mask;
    }
    pix_cells[gid.x] = c;
    if (c == 0xffffffffu) { return; }
    let b = c * ACC;
    atomicAdd(&acc[b + A_FOOT], 1u);
    let r = pcg(gid.x ^ pcg(fr.frame + fr.seed)) >> 21u;
    atomicMax(&acc[b + A_REF], (r << 21u) | k);
}

fn ref_px(c: u32) -> u32 { return atomicLoad(&acc[c * ACC + A_REF]) & 0x1fffffu; }

fn freq_of(c: u32, s: u32) -> f32 {
    let b = c * ACC;
    let n = f32(atomicLoad(&acc[b + A_NS]) + cells[c].ns_prev);
    if (n == 0.0) { return 0.0; }
    let sh = select(0u, 16u, (s & 1u) == 1u);
    let cur = (atomicLoad(&acc[b + A_FREQ + (s >> 1u)]) >> sh) & 0xffffu;
    let prev = (cells[c].freq_prev[s >> 1u] >> sh) & 0xffffu;
    return min(f32(cur + prev) / n, 1.0);
}

fn clear_cell(c: u32) {
    let b = c * ACC;
    cells[c].n = 0.0;
    cells[c].lmean = 0.0;
    cells[c].m2 = 0.0;
    cells[c].mean = vec3<f32>(0.0);
    cells[c].rn = 0.0;
    cells[c].rl = 0.0;
    cells[c].rm2 = 0.0;
    cells[c].ns_prev = 0u;
    cells[c].deps_prev = vec2<u32>(0u);
    for (var w = 0u; w < 18u; w++) {
        cells[c].freq_prev[w] = 0u;
        atomicStore(&acc[b + A_FREQ + w], 0u);
    }
    atomicStore(&acc[b + A_DEPS], 0u);
    atomicStore(&acc[b + A_DEPS + 1u], 0u);
    atomicStore(&acc[b + A_NS], 0u);
}

fn push_tasks(c: u32, m: u32, kind: u32) {
    let base = atomicAdd(&ctr[C_TASKS], m);
    let px = ref_px(c);
    for (var i = 0u; i < m; i++) {
        tasks[base + i] = vec4<u32>(c, px, pcg(base + i + fr.frame * 0x9e3779b9u), kind);
    }
}

// remise à zéro des compteurs de frame (le nombre de cellules et la dette persistent)
@compute @workgroup_size(64)
fn reset_ctr(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= 256u || i == C_CELLS || i == C_DEBT) { return; }
    atomicStore(&ctr[i], 0u);
}

// suspects : dépendances L2 ET (géométrie, ou effet énergétique attendu > 1 %)
@compute @workgroup_size(64)
fn event_mark(@builtin(global_invocation_id) gid: vec3<u32>) {
    let c = gid.x;
    if (c >= min(atomicLoad(&ctr[C_CELLS]), fr.cell_cap)) { return; }
    if (cells[c].n == 0.0) { return; }
    let b = c * ACC;
    let deps = vec2<u32>(atomicLoad(&acc[b + A_DEPS]), atomicLoad(&acc[b + A_DEPS + 1u])) | cells[c].deps_prev;
    let evl2 = vec2<u32>(ev[108], ev[109]);
    if (all((deps & evl2) == vec2<u32>(0u))) { return; }
    let geometry = (fr.flags & F_GEOMETRY) != 0u;
    if (!geometry) {
        var dl = 0.0;
        for (var s = 0u; s < 36u; s++) {
            let m = bitcast<f32>(ev[110u + s]);
            if (m > 0.0) { dl += freq_of(c, s) * m; }
        }
        if (dl <= EPS_ENERGY * max(cells[c].lmean, 0.1)) { return; }
    }
    atomicAdd(&ctr[C_SUSPECTS], 1u);
    if (atomicLoad(&acc[b + A_FOOT]) == 0u) {
        clear_cell(c); // invisible : on ne peut pas le mesurer, ça ne coûte rien
        atomicAdd(&ctr[C_HARD], 1u);
        return;
    }
    atomicStore(&acc[b + A_SUSP], 1u);
    push_tasks(c, select(M_DELTA, M_DELTA0, geometry), 1u);
}

// arguments de dispatch indirect : round 0 = delta tour 1, 1 = delta tour 2, 2 = ordinaires
fn prep_indirect(round: u32) {
    let cur = atomicLoad(&ctr[C_TASKS]);
    var lo = 0u;
    if (round == 0u) {
        atomicStore(&ctr[C_D1], cur);
    } else if (round == 1u) {
        lo = atomicLoad(&ctr[C_D1]);
        atomicStore(&ctr[C_D2], cur);
    } else {
        lo = atomicLoad(&ctr[C_REG0]);
    }
    let n = cur - lo;
    ind[round * 3u] = (8u * ((n + 7u) / 8u) + 63u) / 64u;
    ind[round * 3u + 1u] = 1u;
    ind[round * 3u + 2u] = 1u;
}

// passes « par cellule » : autant de groupes que de cellules réelles, pas que de capacité
@compute @workgroup_size(1)
fn prep_cells() {
    let n = min(atomicLoad(&ctr[C_CELLS]), fr.cell_cap);
    ind[9] = (n + 63u) / 64u;
    ind[10] = 1u;
    ind[11] = 1u;
}

@compute @workgroup_size(1)
fn prep_d1() { prep_indirect(0u); }
@compute @workgroup_size(1)
fn prep_d2() { prep_indirect(1u); }
@compute @workgroup_size(1)
fn prep_reg() { prep_indirect(2u); }

// chemins corrélés : même graine dans la scène courante et la précédente
@compute @workgroup_size(64)
fn delta_trace1(@builtin(global_invocation_id) gid: vec3<u32>) { delta_trace(gid, 0u); }
@compute @workgroup_size(64)
fn delta_trace2(@builtin(global_invocation_id) gid: vec3<u32>) { delta_trace(gid, 1u); }

fn delta_trace(gid: vec3<u32>, round: u32) {
    var lo = 0u;
    var hi = atomicLoad(&ctr[C_D1]);
    if (round == 1u) { lo = hi; hi = atomicLoad(&ctr[C_D2]); }
    let i = lo + stride_task(gid.x, hi - lo);
    if (i >= hi) { return; }
    let t = tasks[i];
    let c = t.x;
    let g = gbuf[t.y];
    let geometry = (fr.flags & F_GEOMETRY) != 0u;
    reset_rec();
    fine_dda = true;
    rec_on = true;
    uni = true;
    split = !geometry;
    sel = 0u;
    seed_rng(t.z, i, fr.frame);
    let s0 = rng;
    let r = radiance(g.p, g.n, g.albedo);
    var x = r.l;
    var y = r.l + r.dl;
    var affected = (m_lights & fr.ev_lights) != 0u;
    var retraced = 0u;
    if (geometry) {
        affected = affected || m_aff;
        y = x;
        if (affected) {
            let keep_trav = m_trav;
            let keep_vsup = m_vsup;
            rec_on = false;
            split = false;
            sel = 1u;
            rng = s0;
            y = radiance(g.p, g.n, g.albedo).l;
            sel = 0u;
            rec_on = true;
            m_trav = keep_trav;
            m_vsup = keep_vsup;
            retraced = 1u;
        }
    }
    commit_deps(c);
    let b = c * ACC;
    atomicAdd(&acc[b + A_PM], 1u);
    if (any(x != y)) { atomicStore(&acc[b + A_PDIFF], 1u); }
    if (affected) { atomicStore(&acc[b + A_PAFF], 1u); }
    add_f32(b + A_PX, x.x);
    add_f32(b + A_PX + 1u, x.y);
    add_f32(b + A_PX + 2u, x.z);
    add_f32(b + A_PY, y.x);
    add_f32(b + A_PY + 1u, y.y);
    add_f32(b + A_PY + 2u, y.z);
    let xl = lum(x);
    let yl = lum(y);
    add_f32(b + A_PXL, xl);
    add_f32(b + A_PYL, yl);
    add_f32(b + A_PXL2, xl * xl);
    add_f32(b + A_PYL2, yl * yl);
    add_f32(b + A_PXYL, xl * yl);
    atomicAdd(&ctr[C_DPATHS], 1u + retraced);
    atomicAdd(&ctr[C_DOLD], retraced);
}

// 2e tour (géométrie) : M_DELTA chemins pour les cellules dont un chemin était affecté
@compute @workgroup_size(64)
fn delta_more(@builtin(global_invocation_id) gid: vec3<u32>) {
    let c = gid.x;
    if (c >= min(atomicLoad(&ctr[C_CELLS]), fr.cell_cap)) { return; }
    let b = c * ACC;
    if (atomicLoad(&acc[b + A_SUSP]) == 0u || atomicLoad(&acc[b + A_PAFF]) == 0u) { return; }
    push_tasks(c, M_DELTA - M_DELTA0, 1u);
}

// variable de contrôle : μ = x̄ − β (ȳ − μ_cache)
@compute @workgroup_size(64)
fn delta_apply(@builtin(global_invocation_id) gid: vec3<u32>) {
    let c = gid.x;
    if (c >= min(atomicLoad(&ctr[C_CELLS]), fr.cell_cap)) { return; }
    let b = c * ACC;
    if (atomicLoad(&acc[b + A_SUSP]) == 0u) { return; }
    let m = f32(atomicLoad(&acc[b + A_PM]));
    let xr = vec3<f32>(ld_f32(b + A_PX), ld_f32(b + A_PX + 1u), ld_f32(b + A_PX + 2u)) / m;
    let yr = vec3<f32>(ld_f32(b + A_PY), ld_f32(b + A_PY + 1u), ld_f32(b + A_PY + 2u)) / m;
    let xl = ld_f32(b + A_PXL) / m;
    let yl = ld_f32(b + A_PYL) / m;
    if (atomicLoad(&acc[b + A_PDIFF]) == 0u) {
        // cellule inchangée : les nouveaux échantillons sont des échantillons ordinaires
        atomicAdd(&acc[b + A_FN], u32(m));
        add_f32(b + A_FR, xr.x * m);
        add_f32(b + A_FR + 1u, xr.y * m);
        add_f32(b + A_FR + 2u, xr.z * m);
        add_f32(b + A_FL, xl * m);
        add_f32(b + A_FL2, ld_f32(b + A_PXL2));
        atomicAdd(&ctr[C_DZERO], 1u);
        return;
    }
    let sxx = max(ld_f32(b + A_PXL2) - m * xl * xl, 0.0);
    let syy = max(ld_f32(b + A_PYL2) - m * yl * yl, 0.0);
    let sxy = ld_f32(b + A_PXYL) - m * xl * yl;
    let mm = m * (m - 1.0);
    var cell = cells[c];
    var var_hist = 1e30;
    if (cell.n > 1.0) { var_hist = cell.m2 / (cell.n - 1.0) / cell.n; }
    var beta = 0.0;
    if (var_hist < 1e29) {
        let denom = syy / mm + var_hist;
        beta = select(1.0, clamp(sxy / mm / denom, 0.0, 1.0), denom > 0.0);
    }
    var var_mu = max(sxx - 2.0 * beta * sxy + beta * beta * syy, 0.0) / mm;
    if (beta > 0.0) { var_mu += beta * beta * var_hist; }
    let mu = max(xr - (yr - cell.mean) * beta, vec3<f32>(0.0));
    let mu_l = max(xl - beta * (yl - cell.lmean), 0.0);
    var s2 = max(sxx / (m - 1.0), 1e-12);
    if (cell.n > 1.0) { s2 = max(s2, cell.m2 / (cell.n - 1.0) * 1e-3); }
    var n_eff = cell.n + m;
    if (var_mu > 0.0) { n_eff = clamp(s2 / var_mu, min(m, 2.0), cell.n + m); }
    n_eff = min(n_eff, N_POST);
    cell.n = n_eff;
    cell.mean = mu;
    cell.lmean = mu_l;
    cell.m2 = s2 * (n_eff - 1.0);
    cell.rn = 0.0;
    cell.rl = 0.0;
    cell.rm2 = 0.0;
    cell.last = fr.frame;
    cells[c] = cell;
}

fn rel_var(c: u32) -> f32 {
    let cell = cells[c];
    let v = select(0.0, cell.m2 / (cell.n - 1.0), cell.n > 1.0);
    return v / (cell.lmean * cell.lmean + REL_EPS);
}

@compute @workgroup_size(64)
fn alloc_hist(@builtin(global_invocation_id) gid: vec3<u32>) {
    let c = gid.x;
    if (c >= min(atomicLoad(&ctr[C_CELLS]), fr.cell_cap)) { return; }
    let foot = atomicLoad(&acc[c * ACC + A_FOOT]);
    let u = f32(pcg(c ^ pcg(fr.frame * 7u + fr.seed)) >> 8u) * (1.0 / 16777216.0);
    qbuf[c] = vec4<f32>(f32(foot) / f32(K), cells[c].n, rel_var(c), u);
    if (foot == 0u) { return; }
    atomicAdd(&ctr[C_VISIBLE], 1u);
    let ex = explore(c);
    if (ex > 0u) { atomicAdd(&ctr[C_EXPLORE], ex); }
    if (cells[c].n >= 8.0) {
        let bin = u32(clamp(floor((log2(max(rel_var(c), 1e-12)) + 20.0) * 2.0), 0.0, 63.0));
        atomicAdd(&ctr[C_HIST + bin], 1u);
        atomicAdd(&ctr[C_MATURE], 1u);
    }
}

fn post_var(n: f32, rel: f32, prior: f32) -> f32 {
    if (n < 1.0) { return prior; }
    return (KAPPA * prior + (n - 1.0) * rel) / (KAPPA + n - 1.0);
}

// nombre d'échantillons pour un seuil λ de gain marginal f·pv/((n+k)(n+k+1)) ≥ λ
fn quota(c: u32, lambda: f32, prior: f32) -> u32 {
    let q = qbuf[c];
    let foot = q.x;
    if (foot == 0.0) { return 0u; }
    let n = q.y;
    let pv = post_var(n, q.z, prior);
    let t = (-1.0 + sqrt(1.0 + 4.0 * foot * pv / lambda)) * 0.5;
    // arrondi stochastique (même tirage au balayage et à l'émission) : Σ quotas suit la solution continue
    var k = max(floor(t - n + q.w), 0.0);
    if (n < 1.0) { k = max(k, 1.0); }
    return min(u32(k), MAX_PER_CELL);
}

fn explore(c: u32) -> u32 {
    if (atomicLoad(&acc[c * ACC + A_FOOT]) == 0u || cells[c].n == 0.0) { return 0u; }
    return select(0u, 1u, fr.frame - cells[c].last >= EXPLORE_PERIOD);
}

// Allocation : seuil λ de gain marginal tel que Σ quota(λ) = budget. Trois balayages parallèles
// de NCAND candidats log-espacés encadrent λ (précision ≈ 2^(70/31³) ≈ 0,2 %) : à l'état convergé,
// toutes les cellules sont près de l'équilibre et Σ quota varie d'un facteur 2 pour 5 % sur λ.
const NCAND: u32 = 32u;
var<workgroup> wsum: array<atomic<u32>, 32>;

@compute @workgroup_size(1)
fn alloc_prep() {
    var prior = 1.0;
    let mature = atomicLoad(&ctr[C_MATURE]);
    if (mature >= 16u) {
        var cum = 0u;
        for (var bin = 0u; bin < 64u; bin++) {
            cum += atomicLoad(&ctr[C_HIST + bin]);
            if (2u * cum >= mature) {
                prior = max(exp2((f32(bin) + 0.5) * 0.5 - 20.0), PRIOR_FLOOR);
                break;
            }
        }
    }
    let surf = atomicLoad(&ctr[C_SURF]);
    let debt = atomicLoad(&ctr[C_DEBT]) + atomicLoad(&ctr[C_DPATHS]);
    let pay = min(debt, u32(f32(surf) * DEBT_RATE));
    atomicStore(&ctr[C_DEBT], debt - pay);
    scal[1] = prior;
    scal[2] = max(f32(surf - pay) - f32(atomicLoad(&ctr[C_EXPLORE])), 0.0);
    scal[3] = -40.0; // bornes de log2 λ
    scal[4] = 30.0;
}

fn cand(i: u32) -> f32 {
    return exp2(scal[3] + (scal[4] - scal[3]) * f32(i) / f32(NCAND - 1u));
}

@compute @workgroup_size(64)
fn alloc_scan1(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>) {
    alloc_scan(gid, lid, C_SCAN);
}
@compute @workgroup_size(64)
fn alloc_scan2(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>) {
    alloc_scan(gid, lid, C_SCAN + NCAND);
}

@compute @workgroup_size(64)
fn alloc_scan3(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>) {
    alloc_scan(gid, lid, C_SCAN + 2u * NCAND);
}

fn alloc_scan(gid: vec3<u32>, lid: vec3<u32>, base: u32) {
    if (lid.x < NCAND) { atomicStore(&wsum[lid.x], 0u); }
    workgroupBarrier();
    let c = gid.x;
    if (c < min(atomicLoad(&ctr[C_CELLS]), fr.cell_cap)) {
        let prior = scal[1];
        for (var i = 0u; i < NCAND; i++) {
            let q = quota(c, cand(i), prior);
            if (q > 0u) { atomicAdd(&wsum[i], q); }
        }
    }
    workgroupBarrier();
    if (lid.x < NCAND) {
        let v = atomicLoad(&wsum[lid.x]);
        if (v > 0u) { atomicAdd(&ctr[base + lid.x], v); }
    }
}

// garde le plus petit candidat dont la somme tient dans le budget, et resserre l'encadrement
@compute @workgroup_size(1)
fn alloc_pick1() { alloc_pick(C_SCAN); }
@compute @workgroup_size(1)
fn alloc_pick2() { alloc_pick(C_SCAN + NCAND); }
@compute @workgroup_size(1)
fn alloc_pick3() { alloc_pick(C_SCAN + 2u * NCAND); }

fn alloc_pick(base: u32) {
    let budget = scal[2];
    var i = NCAND - 1u;
    for (var j = 0u; j < NCAND; j++) {
        if (f32(atomicLoad(&ctr[base + j])) <= budget) { i = j; break; }
    }
    let lo = scal[3] + (scal[4] - scal[3]) * f32(max(i, 1u) - 1u) / f32(NCAND - 1u);
    let hi = scal[3] + (scal[4] - scal[3]) * f32(i) / f32(NCAND - 1u);
    scal[0] = exp2(hi);
    scal[3] = lo;
    scal[4] = hi;
    atomicStore(&ctr[C_REG0], atomicLoad(&ctr[C_TASKS]));
}

@compute @workgroup_size(64)
fn alloc_emit(@builtin(global_invocation_id) gid: vec3<u32>) {
    let c = gid.x;
    if (c >= min(atomicLoad(&ctr[C_CELLS]), fr.cell_cap)) { return; }
    let k = quota(c, scal[0], scal[1]) + explore(c);
    if (k > 0u) { push_tasks(c, k, 0u); }
}

// mode SHaRC : un chemin par pixel de surface, vers une de ses K cellules au hasard
@compute @workgroup_size(64)
fn sharc_emit(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h || gbuf[k].kind != 2u) { return; }
    let slot = pcg(k ^ pcg(fr.frame + fr.seed)) % K;
    let c = pix_cells[k * K + slot];
    if (c == 0xffffffffu) { return; }
    let i = atomicAdd(&ctr[C_TASKS], 1u);
    tasks[i] = vec4<u32>(c, k, pcg(i + fr.frame * 0x9e3779b9u), 0u);
}

@compute @workgroup_size(64)
fn trace(@builtin(global_invocation_id) gid: vec3<u32>) {
    let lo = atomicLoad(&ctr[C_REG0]);
    let hi = atomicLoad(&ctr[C_TASKS]);
    let i = lo + stride_task(gid.x, hi - lo);
    if (i >= hi) { return; }
    let t = tasks[i];
    let c = t.x;
    let g = gbuf[t.y];
    reset_rec();
    fine_dda = false;
    rec_on = (fr.flags & F_SHARC) == 0u;
    uni = false;
    split = false;
    sel = 0u;
    seed_rng(t.z, i, fr.frame);
    let v = radiance(g.p, g.n, g.albedo).l;
    let b = c * ACC;
    if (rec_on) { commit_deps(c); }
    atomicAdd(&acc[b + A_FN], 1u);
    add_f32(b + A_FR, v.x);
    add_f32(b + A_FR + 1u, v.y);
    add_f32(b + A_FR + 2u, v.z);
    let l = lum(v);
    add_f32(b + A_FL, l);
    add_f32(b + A_FL2, l * l);
}

// fusion des échantillons de la frame, filet de sécurité, vieillissement, remise à zéro
@compute @workgroup_size(64)
fn resolve(@builtin(global_invocation_id) gid: vec3<u32>) {
    let c = gid.x;
    if (c >= min(atomicLoad(&ctr[C_CELLS]), fr.cell_cap)) { return; }
    let b = c * ACC;
    var cell = cells[c];
    let fn_ = f32(atomicLoad(&acc[b + A_FN]));
    if (fn_ > 0.0) {
        let fm = vec3<f32>(ld_f32(b + A_FR), ld_f32(b + A_FR + 1u), ld_f32(b + A_FR + 2u)) / fn_;
        let fl = ld_f32(b + A_FL) / fn_;
        let fm2 = max(ld_f32(b + A_FL2) - fn_ * fl * fl, 0.0);
        if ((fr.flags & F_SHARC) != 0u) {
            let a = max(fn_ / (cell.n + fn_), 1.0 - pow(1.0 - EMA_MIN, fn_));
            cell.mean += (fm - cell.mean) * a;
            cell.lmean = lum(cell.mean);
            cell.n += fn_;
        } else {
            let n = cell.n + fn_;
            let dl = fl - cell.lmean;
            cell.mean += (fm - cell.mean) * (fn_ / n);
            cell.m2 += fm2 + dl * dl * cell.n * fn_ / n;
            cell.lmean += dl * (fn_ / n);
            cell.n = n;
            // fenêtre récente du filet de sécurité
            let rn = cell.rn + fn_;
            let rdl = fl - cell.rl;
            cell.rm2 += fm2 + rdl * rdl * cell.rn * fn_ / rn;
            cell.rl += rdl * (fn_ / rn);
            cell.rn = rn;
            if (cell.rn >= SAFETY_WINDOW) {
                if (cell.n > cell.rn) {
                    let v = max(max(cell.m2 / (cell.n - 1.0), cell.rm2 / (cell.rn - 1.0)), 1e-8);
                    let z = abs(cell.rl - cell.lmean) / sqrt(v / cell.rn + v / cell.n);
                    if (z > Z_SAFETY) {
                        // la fenêtre récente ne garde que la luminance : la couleur est remise à l'échelle
                        cell.mean = cell.mean * (cell.rl / max(cell.lmean, 1e-12));
                        cell.n = cell.rn;
                        cell.lmean = cell.rl;
                        cell.m2 = cell.rm2;
                        atomicAdd(&ctr[C_SAFETY], 1u);
                    }
                }
                cell.rn = 0.0;
                cell.rl = 0.0;
                cell.rm2 = 0.0;
            }
        }
        cell.last = fr.frame;
    }
    if (atomicLoad(&acc[b + A_SUSP]) != 0u) { cell.last = fr.frame; }
    // vieillissement des dépendances (fenêtres alternées de DEP_WINDOW échantillons)
    let ns = atomicLoad(&acc[b + A_NS]);
    if (ns >= DEP_WINDOW) {
        cell.deps_prev = vec2<u32>(atomicLoad(&acc[b + A_DEPS]), atomicLoad(&acc[b + A_DEPS + 1u]));
        cell.ns_prev = ns;
        for (var w = 0u; w < 18u; w++) {
            cell.freq_prev[w] = atomicLoad(&acc[b + A_FREQ + w]);
            atomicStore(&acc[b + A_FREQ + w], 0u);
        }
        atomicStore(&acc[b + A_DEPS], 0u);
        atomicStore(&acc[b + A_DEPS + 1u], 0u);
        atomicStore(&acc[b + A_NS], 0u);
    }
    cells[c] = cell;
    atomicMax(&ctr[C_MAXN], bitcast<u32>(cell.n));
    // remise à zéro des accumulateurs de frame
    atomicStore(&acc[b + A_FOOT], 0u);
    atomicStore(&acc[b + A_REF], 0u);
    atomicStore(&acc[b + A_FN], 0u);
    for (var w = A_FR; w <= A_FL2; w++) { atomicStore(&acc[b + w], 0u); }
    for (var w = A_PM; w <= A_SUSP; w++) { atomicStore(&acc[b + w], 0u); }
}

@compute @workgroup_size(64)
fn display(@builtin(global_invocation_id) gid: vec3<u32>) {
    let k = gid.x;
    if (k >= fr.w * fr.h) { return; }
    let g = gbuf[k];
    var v = vec3<f32>(0.0);
    if (g.kind == 1u) {
        v = g.albedo;
    } else if (g.kind == 2u) {
        var w = 0.0;
        for (var s = 0u; s < K; s++) {
            let c = pix_cells[k * K + s];
            if (c != 0xffffffffu && cells[c].n > 0.0) {
                v += cells[c].mean;
                w += 1.0;
            }
        }
        if (w > 0.0) { v /= w; }
    }
    image[k] = vec4<f32>(v, 0.0);
}

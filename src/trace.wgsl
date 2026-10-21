// Path tracer diffus avec NEE : portage GPU de pt.rs (même logique, en f32).
// Une invocation = une requête (un point de surface, une graine) ; `spp` chemins moyennés.
// Enregistre les dépendances du chemin si flags & 2 : régions traversées (DDA), super-régions
// des sommets, lampes choisies par NEE, objets touchés.

const REGION: f32 = 0.6;
const ORIGIN: vec3<f32> = vec3<f32>(-0.2, -0.2, -0.2);
const DIMS: vec3<i32> = vec3<i32>(72, 6, 8);
const WORDS: u32 = 108u; // 72·6·8 bits / 32
const SUPER_X: i32 = 18;
const MAX_DEPTH: u32 = 8u;
const MISS_LEN: f32 = 30.0;
const EPS: f32 = 1e-4;
const OFFSET: f32 = 1e-4;
const PI: f32 = 3.14159265358979;
const LIGHT_ID_BASE: u32 = 64u;
const MASKS: u32 = 8u; // par requête : lampes, objets[4], super-régions[2], libre

struct Box { mn: vec3<f32>, p0: f32, mx: vec3<f32>, p1: f32, albedo: vec3<f32>, p2: f32 };
struct Light { center: vec3<f32>, hx: f32, hz: f32, on: f32, power: f32, toggle_w: f32, emission: vec3<f32>, p0: f32 };
struct Task { p: vec3<f32>, seed_lo: u32, n: vec3<f32>, seed_hi: u32, albedo: vec3<f32>, p0: u32 };
struct Params { n_tasks: u32, n_boxes: u32, n_lights: u32, flags: u32, spp: u32, a: u32, b: u32, c: u32 };

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> boxes: array<Box>;
@group(0) @binding(2) var<storage, read> lights: array<Light>;
@group(0) @binding(3) var<storage, read> tasks: array<Task>;
@group(0) @binding(4) var<storage, read_write> out_l: array<vec4<f32>>;
@group(0) @binding(5) var<storage, read_write> out_dl: array<vec4<f32>>;
@group(0) @binding(6) var<storage, read_write> out_masks: array<u32>;
@group(0) @binding(7) var<storage, read_write> out_regions: array<u32>;

var<private> rng: u32;
var<private> tid: u32;
var<private> rec_on: bool;
var<private> m_lights: u32;
var<private> m_objs: array<u32, 4>;
var<private> m_vsup: vec2<u32>;

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

// ---------------------------------------------------------------- dépendances
fn cell_of(p: vec3<f32>) -> vec3<i32> {
    return clamp(vec3<i32>(floor((p - ORIGIN) / REGION)), vec3<i32>(0), DIMS - vec3<i32>(1));
}

fn index_of(c: vec3<i32>) -> u32 {
    return u32((c.z * DIMS.y + c.y) * DIMS.x + c.x);
}

fn mark_region(idx: u32) {
    let k = tid * WORDS + (idx >> 5u);
    out_regions[k] = out_regions[k] | (1u << (idx & 31u));
}

fn mark_vertex(p: vec3<f32>) {
    if (!rec_on) { return; }
    let c = cell_of(p);
    let s = u32((c.z / 4) * SUPER_X + c.x / 4);
    if (s < 32u) { m_vsup.x = m_vsup.x | (1u << s); } else { m_vsup.y = m_vsup.y | (1u << (s - 32u)); }
}

fn mark_object(id: u32) {
    if (!rec_on) { return; }
    m_objs[id >> 5u] = m_objs[id >> 5u] | (1u << (id & 31u));
}

// DDA d'Amanatides–Woo sur la grille de régions, segment [a, b].
fn mark_segment(a: vec3<f32>, b: vec3<f32>) {
    if (!rec_on) { return; }
    let d = b - a;
    let lo = ORIGIN;
    let hi = ORIGIN + vec3<f32>(DIMS) * REGION;
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
    var c = cell_of(p);
    var stp = vec3<i32>(0);
    var tmax = vec3<f32>(1e30);
    var tdelta = vec3<f32>(1e30);
    for (var ax = 0; ax < 3; ax++) {
        if (abs(d[ax]) >= 1e-12) {
            let pos = d[ax] > 0.0;
            stp[ax] = select(-1, 1, pos);
            let boundary = ORIGIN[ax] + f32(c[ax] + select(0, 1, pos)) * REGION;
            tmax[ax] = t0 + (boundary - p[ax]) / d[ax];
            tdelta[ax] = REGION / abs(d[ax]);
        }
    }
    for (var it = 0; it < 256; it++) {
        mark_region(index_of(c));
        var ax = 2;
        if (tmax.x < tmax.y) {
            if (tmax.x < tmax.z) { ax = 0; }
        } else if (tmax.y < tmax.z) {
            ax = 1;
        }
        if (tmax[ax] > t1) { break; }
        c[ax] += stp[ax];
        if (c[ax] < 0 || c[ax] >= DIMS[ax]) { break; }
        tmax[ax] += tdelta[ax];
    }
}

// ---------------------------------------------------------------- géométrie
struct Hit { found: bool, t: f32, p: vec3<f32>, n: vec3<f32>, albedo: vec3<f32>, id: u32, is_light: bool };

// évite 1/0 : une composante quasi nulle devient ±1e-12 (son signe est conservé)
fn safe_dir(d: vec3<f32>) -> vec3<f32> {
    let s = select(vec3<f32>(1.0), vec3<f32>(-1.0), d < vec3<f32>(0.0));
    return select(d, s * 1e-12, abs(d) < vec3<f32>(1e-12));
}

// (t0, t1, axe) du test des dalles
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
    h.is_light = false;
    let inv = 1.0 / safe_dir(d);
    var axis = 0u;
    for (var i = 0u; i < params.n_boxes; i++) {
        let s = slab(boxes[i].mn, boxes[i].mx, o, inv);
        if (s.x <= s.y && s.x > EPS && s.x < h.t) {
            h.t = s.x;
            h.id = i;
            axis = u32(s.z);
            h.found = true;
        }
    }
    for (var j = 0u; j < params.n_lights; j++) {
        if (abs(d.y) < 1e-12) { continue; }
        let l = lights[j];
        let t = (l.center.y - o.y) / d.y;
        if (t > EPS && t < h.t) {
            let p = o + d * t;
            if (abs(p.x - l.center.x) <= l.hx && abs(p.z - l.center.z) <= l.hz) {
                h.t = t;
                h.id = LIGHT_ID_BASE + j;
                h.is_light = true;
                h.found = true;
            }
        }
    }
    if (h.found) {
        h.p = o + d * h.t;
        if (h.is_light) {
            h.n = select(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, -1.0, 0.0), d.y > 0.0);
            h.albedo = vec3<f32>(0.0);
        } else {
            var nn = vec3<f32>(0.0);
            nn[axis] = select(1.0, -1.0, d[axis] > 0.0);
            h.n = nn;
            h.albedo = boxes[h.id].albedo;
        }
    }
    return h;
}

// Premier objet qui bloque [o, o + d·tmax] : (trouvé, id, t)
struct Occ { found: bool, id: u32, t: f32 };

fn occluder(o: vec3<f32>, d: vec3<f32>, tmax: f32) -> Occ {
    var r: Occ;
    r.found = false;
    r.t = tmax;
    let inv = 1.0 / safe_dir(d);
    for (var i = 0u; i < params.n_boxes; i++) {
        let s = slab(boxes[i].mn, boxes[i].mx, o, inv);
        if (s.x <= s.y && s.x > EPS && s.x < r.t && s.y > 0.0) {
            r.found = true;
            r.id = i;
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

// ---------------------------------------------------------------- transport
fn light_power(j: u32, uni: bool) -> f32 {
    if (uni || lights[j].on > 0.5) { return lights[j].power; }
    return 0.0;
}

struct LL { l: vec3<f32>, dl: vec3<f32> };

fn radiance(p0: vec3<f32>, n0: vec3<f32>, a0: vec3<f32>, uni: bool) -> LL {
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
        // NEE (choix de lampe ∝ puissance ; mode union : indépendant de l'état allumé)
        var total = 0.0;
        for (var j = 0u; j < params.n_lights; j++) { total += light_power(j, uni); }
        if (total > 0.0) {
            var u = rnd() * total;
            var sel = params.n_lights;
            var pmf = 0.0;
            for (var j = 0u; j < params.n_lights; j++) {
                let pw = light_power(j, uni);
                if (pw > 0.0 && (u < pw || j == params.n_lights - 1u)) {
                    sel = j;
                    pmf = pw / total;
                    break;
                }
                u -= pw;
            }
            if (sel < params.n_lights) {
                if (rec_on) { m_lights = m_lights | (1u << sel); }
                let li = lights[sel];
                let q = li.center + vec3<f32>((2.0 * rnd() - 1.0) * li.hx, 0.0, (2.0 * rnd() - 1.0) * li.hz);
                let w = q - origin;
                let dist2 = dot(w, w);
                let dist = sqrt(dist2);
                let wd = w / dist;
                let cos_x = dot(n, wd);
                let cos_l = wd.y;
                let on = li.on > 0.5;
                if (cos_x > 0.0 && cos_l > 0.0 && !on && li.toggle_w == 0.0) {
                    mark_segment(origin, q);
                } else if (cos_x > 0.0 && cos_l > 0.0) {
                    let occ = occluder(origin, wd, dist - 1e-4);
                    if (occ.found) {
                        mark_segment(origin, origin + wd * occ.t);
                        mark_object(occ.id);
                    } else {
                        mark_segment(origin, q);
                        mark_object(LIGHT_ID_BASE + sel);
                        let g = cos_x * cos_l / dist2 * (4.0 * li.hx * li.hz) / pmf / PI;
                        let c = beta * albedo * li.emission * g;
                        if (on) { res.l += c; }
                        res.dl += c * li.toggle_w;
                    }
                }
            }
        }
        // rebond diffus
        let d = cosine_dir(n);
        beta = beta * albedo;
        let h = intersect(origin, d);
        if (!h.found) {
            mark_segment(origin, origin + d * MISS_LEN);
            break;
        }
        mark_segment(origin, h.p);
        mark_object(h.id);
        if (h.is_light) { break; }
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

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= params.n_tasks) { return; }
    tid = i;
    let t = tasks[i];
    let uni = (params.flags & 1u) != 0u;
    rec_on = (params.flags & 2u) != 0u;
    rng = pcg(t.seed_lo ^ pcg(t.seed_hi + 0x9e3779b9u));
    m_lights = 0u;
    m_objs = array<u32, 4>(0u, 0u, 0u, 0u);
    m_vsup = vec2<u32>(0u);
    if (rec_on) {
        for (var w = 0u; w < WORDS; w++) { out_regions[i * WORDS + w] = 0u; }
    }
    var acc = vec3<f32>(0.0);
    var accd = vec3<f32>(0.0);
    for (var s = 0u; s < params.spp; s++) {
        let r = radiance(t.p, t.n, t.albedo, uni);
        acc += r.l;
        accd += r.dl;
    }
    let inv = 1.0 / f32(params.spp);
    out_l[i] = vec4<f32>(acc * inv, 0.0);
    out_dl[i] = vec4<f32>(accd * inv, 0.0);
    if (rec_on) {
        let b = i * MASKS;
        out_masks[b] = m_lights;
        out_masks[b + 1u] = m_objs[0];
        out_masks[b + 2u] = m_objs[1];
        out_masks[b + 3u] = m_objs[2];
        out_masks[b + 4u] = m_objs[3];
        out_masks[b + 5u] = m_vsup.x;
        out_masks[b + 6u] = m_vsup.y;
    }
}

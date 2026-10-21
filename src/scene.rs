use crate::math::{v3, Rng, V3};

#[derive(Clone, Copy, Debug)]
pub struct Aabb {
    pub min: V3,
    pub max: V3,
}

impl Aabb {
    pub fn union(self, o: Aabb) -> Aabb {
        Aabb { min: self.min.min(o.min), max: self.max.max(o.max) }
    }
    pub fn expand(self, e: f64) -> Aabb {
        Aabb { min: self.min - V3::splat(e), max: self.max + V3::splat(e) }
    }
}

pub struct Object {
    pub aabb: Aabb,
    pub albedo: V3,
}

/// Lampe rectangulaire horizontale émettant vers le bas.
pub struct Light {
    pub center: V3,
    pub hx: f64,
    pub hz: f64,
    pub emission: V3,
    pub on: bool,
}

impl Light {
    pub fn area(&self) -> f64 {
        4.0 * self.hx * self.hz
    }
    pub fn aabb(&self) -> Aabb {
        let e = v3(self.hx, 0.02, self.hz);
        Aabb { min: self.center - e, max: self.center + e }
    }
}

/// Identifiants pour les empreintes « objets touchés » : objet i -> i, lampe j -> LIGHT_ID_BASE + j.
pub const LIGHT_ID_BASE: usize = 32;
pub const MOVING_BOX: usize = 0;

pub struct Hit {
    pub t: f64,
    pub p: V3,
    pub n: V3,
    pub albedo: V3,
    pub id: usize,
    pub light: Option<usize>,
}

pub struct Scene {
    pub objects: Vec<Object>,
    pub lights: Vec<Light>,
}

/// État discret de la scène : ce qui change au cours de la séquence.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct State {
    pub box_step: u32,
    pub l0_on: bool,
    pub l2_on: bool,
}

impl State {
    pub fn id(&self) -> String {
        format!("b{:02}_l0{}_l2{}", self.box_step, self.l0_on as u8, self.l2_on as u8)
    }
}

pub const FRAMES: u32 = 120;

pub fn state_at(frame: u32) -> State {
    let box_step = if frame < 40 {
        0
    } else if frame < 64 {
        (frame - 40) / 2 + 1
    } else {
        12
    };
    State { box_step, l0_on: frame < 100, l2_on: frame >= 80 }
}

pub fn moving_box_aabb(step: u32) -> Aabb {
    let x0 = 0.8 + step as f64 * (2.0 / 12.0);
    Aabb { min: v3(x0, 0.0, 2.4), max: v3(x0 + 0.6, 0.9, 3.0) }
}

fn bx(min: V3, max: V3, albedo: V3) -> Object {
    Object { aabb: Aabb { min, max }, albedo }
}

/// Deux pièces vues en coupe (« maison de poupée »), reliées par une porte.
/// Pièce A : x ∈ [0, 5.1], pièce B : x ∈ [5.3, 10.4], y ∈ [0, 3], z ∈ [0, 4], face avant ouverte.
pub fn build(s: &State) -> Scene {
    let white = V3::splat(0.75);
    let red = v3(0.75, 0.2, 0.15);
    let green = v3(0.15, 0.6, 0.2);
    let objects = vec![
        bx(moving_box_aabb(s.box_step).min, moving_box_aabb(s.box_step).max, v3(0.2, 0.3, 0.8)),
        bx(v3(-0.1, -0.1, -0.1), v3(10.5, 0.0, 4.0), white), // sol
        bx(v3(-0.1, 3.0, -0.1), v3(10.5, 3.1, 4.0), white),  // plafond
        bx(v3(-0.1, 0.0, -0.1), v3(10.5, 3.0, 0.0), white),  // fond
        bx(v3(-0.1, 0.0, 0.0), v3(0.0, 3.0, 4.0), red),      // mur gauche
        bx(v3(10.4, 0.0, 0.0), v3(10.5, 3.0, 4.0), green),   // mur droit
        bx(v3(5.1, 0.0, 0.0), v3(5.3, 3.0, 1.5), white),     // cloison
        bx(v3(5.1, 0.0, 2.5), v3(5.3, 3.0, 4.0), white),
        bx(v3(5.1, 2.2, 1.5), v3(5.3, 3.0, 2.5), white),     // linteau de la porte
        bx(v3(3.6, 0.0, 0.6), v3(4.2, 1.8, 1.2), white),     // colonne pièce A
        bx(v3(6.2, 0.0, 1.0), v3(7.0, 0.7, 1.8), v3(0.8, 0.7, 0.3)),
        bx(v3(8.8, 0.0, 2.6), v3(9.6, 1.2, 3.2), white),
    ];
    let lights = vec![
        Light { center: v3(2.55, 2.98, 1.8), hx: 0.5, hz: 0.5, emission: V3::splat(8.0), on: s.l0_on },
        Light { center: v3(7.85, 2.98, 2.0), hx: 0.35, hz: 0.35, emission: V3::splat(5.0), on: true },
        Light { center: v3(9.2, 2.98, 1.0), hx: 0.3, hz: 0.3, emission: v3(40.0, 30.0, 18.0), on: s.l2_on },
    ];
    Scene { objects, lights }
}

const EPS: f64 = 1e-7;

fn slab(b: &Aabb, o: V3, inv: V3) -> Option<(f64, f64, usize)> {
    let mut t0 = f64::NEG_INFINITY;
    let mut t1 = f64::INFINITY;
    let mut axis = 0;
    for a in 0..3 {
        let (mut ta, mut tb) = ((b.min.get(a) - o.get(a)) * inv.get(a), (b.max.get(a) - o.get(a)) * inv.get(a));
        if ta > tb {
            std::mem::swap(&mut ta, &mut tb);
        }
        if ta > t0 {
            t0 = ta;
            axis = a;
        }
        t1 = t1.min(tb);
    }
    if t0 <= t1 { Some((t0, t1, axis)) } else { None }
}

fn axis_normal(axis: usize, d: V3) -> V3 {
    let s = if d.get(axis) > 0.0 { -1.0 } else { 1.0 };
    match axis {
        0 => v3(s, 0.0, 0.0),
        1 => v3(0.0, s, 0.0),
        _ => v3(0.0, 0.0, s),
    }
}

impl Scene {
    pub fn intersect(&self, o: V3, d: V3, tmax: f64) -> Option<Hit> {
        let inv = v3(1.0 / d.x, 1.0 / d.y, 1.0 / d.z);
        let mut best: Option<(f64, usize, usize)> = None; // (t, id, axis)
        let mut tb = tmax;
        for (i, ob) in self.objects.iter().enumerate() {
            if let Some((t0, _, axis)) = slab(&ob.aabb, o, inv) {
                if t0 > EPS && t0 < tb {
                    tb = t0;
                    best = Some((t0, i, axis));
                }
            }
        }
        let mut best_light = None;
        for (j, l) in self.lights.iter().enumerate() {
            if d.y.abs() < 1e-12 {
                continue;
            }
            let t = (l.center.y - o.y) / d.y;
            if t > EPS && t < tb {
                let p = o + d * t;
                if (p.x - l.center.x).abs() <= l.hx && (p.z - l.center.z).abs() <= l.hz {
                    tb = t;
                    best_light = Some((t, j));
                }
            }
        }
        if let Some((t, j)) = best_light {
            let n = if d.y > 0.0 { v3(0.0, -1.0, 0.0) } else { v3(0.0, 1.0, 0.0) };
            return Some(Hit { t, p: o + d * t, n, albedo: V3::default(), id: LIGHT_ID_BASE + j, light: Some(j) });
        }
        best.map(|(t, i, axis)| Hit {
            t,
            p: o + d * t,
            n: axis_normal(axis, d),
            albedo: self.objects[i].albedo,
            id: i,
            light: None,
        })
    }

    /// Renvoie l'objet qui bloque le segment [o, o + d·tmax], s'il y en a un.
    pub fn occluder(&self, o: V3, d: V3, tmax: f64) -> Option<usize> {
        let inv = v3(1.0 / d.x, 1.0 / d.y, 1.0 / d.z);
        for (i, ob) in self.objects.iter().enumerate() {
            if let Some((t0, t1, _)) = slab(&ob.aabb, o, inv) {
                if t0 > EPS && t0 < tmax && t1 > 0.0 {
                    return Some(i);
                }
            }
        }
        None
    }

    pub fn light_power(&self, j: usize) -> f64 {
        let l = &self.lights[j];
        if l.on { l.emission.lum() * l.area() } else { 0.0 }
    }
}

/// Base orthonormée autour de n.
pub fn frame(n: V3) -> (V3, V3) {
    let a = if n.x.abs() > 0.9 { v3(0.0, 1.0, 0.0) } else { v3(1.0, 0.0, 0.0) };
    let t = n.cross(a).norm();
    (t, n.cross(t))
}

pub fn cosine_dir(n: V3, rng: &mut Rng) -> V3 {
    let (t, b) = frame(n);
    let r = rng.f64().sqrt();
    let phi = 2.0 * std::f64::consts::PI * rng.f64();
    let z = (1.0 - r * r).max(0.0).sqrt();
    (t * (r * phi.cos()) + b * (r * phi.sin()) + n * z).norm()
}

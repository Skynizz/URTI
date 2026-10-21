//! Grille grossière de régions et ensembles de régions traversées (dépendances spatiales).
use crate::math::{v3, V3};
use crate::scene::Aabb;

/// 0.6 m avec origine -0.2 : la cloison (x = 5.2) tombe sur une frontière de région.
pub const REGION: f64 = 0.6;
pub const ORIGIN: V3 = v3(-0.2, -0.2, -0.2);
pub const DIMS: [i64; 3] = [72, 6, 8]; // jusqu'à 8 pièces
pub const WORDS: usize = 54;

/// Ensemble exact de régions (jusqu'à 3456).
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Bits(pub [u64; WORDS]);

impl Default for Bits {
    fn default() -> Bits {
        Bits([0; WORDS])
    }
}

impl Bits {
    pub fn set(&mut self, i: usize) {
        self.0[i >> 6] |= 1 << (i & 63);
    }
    pub fn or(&mut self, o: &Bits) {
        for k in 0..WORDS {
            self.0[k] |= o.0[k];
        }
    }
    pub fn intersects(&self, o: &Bits) -> bool {
        (0..WORDS).any(|k| self.0[k] & o.0[k] != 0)
    }
}

/// Super-régions pour les fréquences : blocs de 4 × 6 × 4 régions (2,4 m × toute la hauteur ×
/// 2,4 m). Un hachage sur 256 compteurs a échoué : une cellule traverse 100 à 300 régions par
/// fenêtre, les collisions rendaient toutes les fréquences élevées.
const SUPER_DIV: [i64; 3] = [4, 6, 4];
const SUPER_DIMS: [usize; 3] = [18, 1, 2];
pub const SUPER: usize = SUPER_DIMS[0] * SUPER_DIMS[1] * SUPER_DIMS[2];

pub fn super_of(region: usize) -> usize {
    let r = region as i64;
    let (x, y, z) = (r % DIMS[0], r / DIMS[0] % DIMS[1], r / (DIMS[0] * DIMS[1]));
    let (sx, sy, sz) = ((x / SUPER_DIV[0]) as usize, (y / SUPER_DIV[1]) as usize, (z / SUPER_DIV[2]) as usize);
    (sz * SUPER_DIMS[1] + sy) * SUPER_DIMS[0] + sx
}

/// Fréquences exactes par super-région : nombre d'échantillons de la fenêtre qui y ont un sommet
/// (compteurs saturants).
#[derive(Clone, Copy)]
pub struct Freq {
    pub cnt: [u8; SUPER],
    pub n: u16,
}

impl Default for Freq {
    fn default() -> Freq {
        Freq { cnt: [0; SUPER], n: 0 }
    }
}

impl Freq {
    pub fn add(&mut self, vsup: u64) {
        for (s, c) in self.cnt.iter_mut().enumerate() {
            *c = c.saturating_add((vsup >> s & 1) as u8);
        }
        self.n = self.n.saturating_add(1);
    }
}

pub fn region_of(p: V3) -> usize {
    index(cell_of(p))
}

fn index(c: [i64; 3]) -> usize {
    ((c[2] * DIMS[1] + c[1]) * DIMS[0] + c[0]) as usize
}

fn cell_of(p: V3) -> [i64; 3] {
    let mut c = [0i64; 3];
    for a in 0..3 {
        c[a] = (((p.get(a) - ORIGIN.get(a)) / REGION).floor() as i64).clamp(0, DIMS[a] - 1);
    }
    c
}

pub fn mark_aabb(bits: &mut Bits, b: &Aabb) {
    let (lo, hi) = (cell_of(b.min), cell_of(b.max));
    for z in lo[2]..=hi[2] {
        for y in lo[1]..=hi[1] {
            for x in lo[0]..=hi[0] {
                bits.set(index([x, y, z]));
            }
        }
    }
}

/// Marque toutes les régions traversées par le segment [a, b] (DDA d'Amanatides–Woo).
pub fn mark_segment(bits: &mut Bits, a: V3, b: V3) {
    let d = b - a;
    let (mut t0, mut t1) = (0.0f64, 1.0f64);
    for ax in 0..3 {
        let lo = ORIGIN.get(ax);
        let hi = lo + DIMS[ax] as f64 * REGION;
        let (o, dd) = (a.get(ax), d.get(ax));
        if dd.abs() < 1e-12 {
            if o < lo || o > hi {
                return;
            }
        } else {
            let (mut ta, mut tb) = ((lo - o) / dd, (hi - o) / dd);
            if ta > tb {
                std::mem::swap(&mut ta, &mut tb);
            }
            t0 = t0.max(ta);
            t1 = t1.min(tb);
        }
    }
    if t0 > t1 {
        return;
    }
    let p = a + d * t0;
    let mut c = cell_of(p);
    let mut step = [0i64; 3];
    let mut tmax = [f64::INFINITY; 3];
    let mut tdelta = [f64::INFINITY; 3];
    for ax in 0..3 {
        let dd = d.get(ax);
        if dd.abs() < 1e-12 {
            continue;
        }
        step[ax] = if dd > 0.0 { 1 } else { -1 };
        let boundary = ORIGIN.get(ax) + (c[ax] + (dd > 0.0) as i64) as f64 * REGION;
        tmax[ax] = t0 + (boundary - p.get(ax)) / dd;
        tdelta[ax] = REGION / dd.abs();
    }
    loop {
        bits.set(index(c));
        let ax = if tmax[0] < tmax[1] {
            if tmax[0] < tmax[2] { 0 } else { 2 }
        } else if tmax[1] < tmax[2] {
            1
        } else {
            2
        };
        if tmax[ax] > t1 {
            break;
        }
        c[ax] += step[ax];
        if c[ax] < 0 || c[ax] >= DIMS[ax] {
            break;
        }
        tmax[ax] += tdelta[ax];
    }
}

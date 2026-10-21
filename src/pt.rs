//! Path tracer diffus avec NEE, instrumenté pour enregistrer les dépendances d'un chemin.
use crate::deps::{mark_segment, Bits};
use crate::math::{Rng, V3};
use crate::scene::{cosine_dir, Scene, LIGHT_ID_BASE};

const MAX_DEPTH: u32 = 8;
const MISS_LEN: f64 = 30.0;

pub trait Rec {
    fn segment(&mut self, a: V3, b: V3);
    fn object(&mut self, id: usize);
}

pub struct NoRec;
impl Rec for NoRec {
    fn segment(&mut self, _: V3, _: V3) {}
    fn object(&mut self, _: usize) {}
}

/// Dépendances d'un chemin : régions traversées (H*) et objets touchés (H0).
#[derive(Default)]
pub struct DepRec {
    pub regions: Bits,
    pub objs: u64,
}
impl Rec for DepRec {
    fn segment(&mut self, a: V3, b: V3) {
        mark_segment(&mut self.regions, a, b);
    }
    fn object(&mut self, id: usize) {
        self.objs |= 1 << id;
    }
}

fn sample_light(scene: &Scene, rng: &mut Rng) -> Option<(usize, f64)> {
    let total: f64 = (0..scene.lights.len()).map(|j| scene.light_power(j)).sum();
    if total <= 0.0 {
        return None;
    }
    let mut u = rng.f64() * total;
    for j in 0..scene.lights.len() {
        let p = scene.light_power(j);
        if p > 0.0 && (u < p || j == scene.lights.len() - 1) {
            return Some((j, p / total));
        }
        u -= p;
    }
    None
}

/// Estimateur à un échantillon de la radiance sortante (diffuse) au point p de normale n.
pub fn radiance<R: Rec>(scene: &Scene, p0: V3, n0: V3, albedo0: V3, rng: &mut Rng, rec: &mut R) -> V3 {
    let (mut p, mut n, mut albedo) = (p0, n0, albedo0);
    let mut beta = V3::splat(1.0);
    let mut l = V3::default();
    for depth in 0..MAX_DEPTH {
        let origin = p + n * 1e-6;
        // NEE
        if let Some((j, pmf)) = sample_light(scene, rng) {
            let li = &scene.lights[j];
            let q = li.center + crate::math::v3((2.0 * rng.f64() - 1.0) * li.hx, 0.0, (2.0 * rng.f64() - 1.0) * li.hz);
            let w = q - origin;
            let dist2 = w.dot(w);
            let dist = dist2.sqrt();
            let wd = w / dist;
            let (cos_x, cos_l) = (n.dot(wd), wd.y);
            rec.segment(origin, q);
            if cos_x > 0.0 && cos_l > 0.0 {
                match scene.occluder(origin, wd, dist - 1e-6) {
                    Some(o) => rec.object(o),
                    None => {
                        rec.object(LIGHT_ID_BASE + j);
                        let g = cos_x * cos_l / dist2 * li.area() / pmf / std::f64::consts::PI;
                        l += beta.mul(albedo).mul(li.emission) * g;
                    }
                }
            }
        }
        // rebond diffus
        let d = cosine_dir(n, rng);
        beta = beta.mul(albedo);
        let hit = scene.intersect(origin, d, f64::INFINITY);
        match hit {
            None => {
                rec.segment(origin, origin + d * MISS_LEN);
                break;
            }
            Some(h) => {
                rec.segment(origin, h.p);
                rec.object(h.id);
                if h.light.is_some() {
                    break; // émission déjà comptée par NEE
                }
                p = h.p;
                n = h.n;
                albedo = h.albedo;
            }
        }
        if depth >= 2 {
            let q = beta.max_c().clamp(0.05, 0.95);
            if rng.f64() > q {
                break;
            }
            beta = beta / q;
        }
    }
    l
}

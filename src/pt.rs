//! Path tracer diffus avec NEE, instrumenté pour enregistrer les dépendances d'un chemin.
use crate::deps::{mark_segment, Bits};
use crate::math::{Rng, V3};
use crate::scene::{cosine_dir, Scene, LIGHT_ID_BASE};

const MAX_DEPTH: u32 = 8;
const MISS_LEN: f64 = 30.0;

pub trait Rec {
    fn segment(&mut self, a: V3, b: V3);
    fn object(&mut self, id: usize);
    fn light_selected(&mut self, _j: usize) {}
    /// sommet du chemin (point où il rebondit) : c'est là qu'un changement de lumière est capté
    fn vertex(&mut self, _p: V3) {}
}

pub struct NoRec;
impl Rec for NoRec {
    fn segment(&mut self, _: V3, _: V3) {}
    fn object(&mut self, _: usize) {}
}

/// Dépendances d'un chemin : régions traversées (H*), objets touchés (H0), lampes choisies par NEE.
#[derive(Default)]
pub struct DepRec {
    pub regions: Bits,
    pub objs: u128,
    pub lights: u64,
    /// régions contenant les sommets du chemin
    pub verts: Bits,
}
impl Rec for DepRec {
    fn segment(&mut self, a: V3, b: V3) {
        mark_segment(&mut self.regions, a, b);
    }
    fn object(&mut self, id: usize) {
        self.objs |= 1u128 << id;
    }
    fn light_selected(&mut self, j: usize) {
        self.lights |= 1 << j;
    }
    fn vertex(&mut self, p: V3) {
        self.verts.set(crate::deps::region_of(p));
    }
}

/// Choix de lampe ∝ puissance. En mode `union`, la pmf ignore l'état allumé/éteint : deux scènes
/// qui ne diffèrent que par une lampe consomment alors les mêmes nombres aléatoires (chemins corrélés).
fn sample_light(scene: &Scene, rng: &mut Rng, union: bool) -> Option<(usize, f64)> {
    let power = |j: usize| if union { scene.light_power_if_on(j) } else { scene.light_power(j) };
    let total: f64 = (0..scene.lights.len()).map(power).sum();
    if total <= 0.0 {
        return None;
    }
    let mut u = rng.f64() * total;
    for j in 0..scene.lights.len() {
        let p = power(j);
        if p > 0.0 && (u < p || j == scene.lights.len() - 1) {
            return Some((j, p / total));
        }
        u -= p;
    }
    None
}

/// Estimateur à un échantillon de la radiance sortante (diffuse) au point p de normale n.
/// La consommation de nombres aléatoires par sommet est fixe, pour l'échantillonnage corrélé.
pub fn radiance<R: Rec>(scene: &Scene, p0: V3, n0: V3, albedo0: V3, rng: &mut Rng, rec: &mut R, union: bool) -> V3 {
    radiance_split(scene, p0, n0, albedo0, rng, rec, union, &[]).0
}

/// Comme `radiance`, et renvoie aussi Σ_j w_j · C_j, où C_j est la contribution NEE qu'aurait la
/// lampe j si elle était allumée (géométrie identique). Pour un changement qui ne touche que des
/// lampes, avec w_j = allumée_avant − allumée_après, l'ancienne radiance vaut L + Σ w_j C_j :
/// le chemin de l'ancienne scène n'a pas besoin d'être retracé.
#[allow(clippy::too_many_arguments)]
pub fn radiance_split<R: Rec>(
    scene: &Scene,
    p0: V3,
    n0: V3,
    albedo0: V3,
    rng: &mut Rng,
    rec: &mut R,
    union: bool,
    toggle_w: &[f64],
) -> (V3, V3) {
    let (mut p, mut n, mut albedo) = (p0, n0, albedo0);
    let mut beta = V3::splat(1.0);
    let mut l = V3::default();
    let mut dl = V3::default();
    rec.vertex(p0);
    for depth in 0..MAX_DEPTH {
        let origin = p + n * 1e-6;
        // NEE
        if let Some((j, pmf)) = sample_light(scene, rng, union) {
            rec.light_selected(j);
            let li = &scene.lights[j];
            let q = li.center + crate::math::v3((2.0 * rng.f64() - 1.0) * li.hx, 0.0, (2.0 * rng.f64() - 1.0) * li.hz);
            let w = q - origin;
            let dist2 = w.dot(w);
            let dist = dist2.sqrt();
            let wd = w / dist;
            let (cos_x, cos_l) = (n.dot(wd), wd.y);
            let wj = toggle_w.get(j).copied().unwrap_or(0.0);
            if cos_x > 0.0 && cos_l > 0.0 && !li.on && wj == 0.0 {
                rec.segment(origin, q); // lampe éteinte (mode union) : dépendance conservatrice
            } else if cos_x > 0.0 && cos_l > 0.0 {
                // un rayon d'ombre bloqué ne dépend que de l'espace jusqu'à l'occulteur
                match scene.occluder(origin, wd, dist - 1e-6) {
                    Some((o, t)) => {
                        rec.segment(origin, origin + wd * t);
                        rec.object(o);
                    }
                    None => {
                        rec.segment(origin, q);
                        rec.object(LIGHT_ID_BASE + j);
                        let g = cos_x * cos_l / dist2 * li.area() / pmf / std::f64::consts::PI;
                        let c = beta.mul(albedo).mul(li.emission) * g;
                        if li.on {
                            l += c;
                        }
                        dl += c * wj;
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
                rec.vertex(h.p);
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
    (l, dl)
}

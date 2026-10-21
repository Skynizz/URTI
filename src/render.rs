//! Caméra, G-buffer, références et métriques.
use crate::math::{mix64, v3, V3};
use crate::par::par_map;
use crate::tracer::{Opts, Req, Tracer};
use crate::scene::Scene;

pub const REF_SPP: usize = 4096;
const HFOV_DEG: f64 = 80.0;

/// Taille de la scène (nombre de pièces) et caméra associée : 96 px de large par pièce,
/// pour garder une densité de pixels constante quand la scène grandit.
#[derive(Clone, Copy)]
pub struct World {
    pub rooms: usize,
    pub w: usize,
    pub h: usize,
}

impl World {
    pub fn new(rooms: usize) -> World {
        World { rooms, w: 96 * rooms, h: 96 }
    }
    pub fn len(&self) -> usize {
        self.w * self.h
    }
    fn eye_look(&self) -> (V3, V3) {
        let cx = 2.6 * self.rooms as f64 - 0.05;
        (v3(cx, 1.7, 4.0 + 3.25 * self.rooms as f64), v3(cx, 1.2, 2.0))
    }
    /// Largeur angulaire d'un pixel (empreinte = distance × px_angle).
    pub fn px_angle(&self) -> f64 {
        2.0 * (HFOV_DEG.to_radians() * 0.5).tan() / self.w as f64
    }
    /// (œil, avant, droite, haut, tan demi-angle horizontal, vertical)
    pub fn camera(&self) -> (V3, V3, V3, V3, f64, f64) {
        let (eye, look) = self.eye_look();
        let f = (look - eye).norm();
        let r = f.cross(v3(0.0, 1.0, 0.0)).norm();
        let th = (HFOV_DEG.to_radians() * 0.5).tan();
        (eye, f, r, r.cross(f), th, th * self.h as f64 / self.w as f64)
    }
    pub fn camera_ray(&self, i: usize, j: usize) -> (V3, V3) {
        let (eye, look) = self.eye_look();
        let f = (look - eye).norm();
        let r = f.cross(v3(0.0, 1.0, 0.0)).norm();
        let u = r.cross(f);
        let th = (HFOV_DEG.to_radians() * 0.5).tan();
        let tv = th * self.h as f64 / self.w as f64;
        let sx = 2.0 * (i as f64 + 0.5) / self.w as f64 - 1.0;
        let sy = 1.0 - 2.0 * (j as f64 + 0.5) / self.h as f64;
        (eye, (f + r * (sx * th) + u * (sy * tv)).norm())
    }
}

#[derive(Clone, Copy)]
pub enum Px {
    Miss,
    Light(V3),
    Surf { p: V3, n: V3, albedo: V3, dist: f64 },
}

pub fn gbuffer(scene: &Scene, world: &World) -> Vec<Px> {
    par_map(world.len(), |k| {
        let (o, d) = world.camera_ray(k % world.w, k / world.w);
        match scene.intersect(o, d, f64::INFINITY) {
            None => Px::Miss,
            Some(h) => match h.light {
                Some(j) => {
                    let l = &scene.lights[j];
                    Px::Light(if l.on && d.y > 0.0 { l.emission } else { V3::default() })
                }
                None => Px::Surf { p: h.p, n: h.n, albedo: h.albedo, dist: h.t },
            },
        }
    })
}

/// Image de référence : moyenne de REF_SPP chemins par pixel au point visible.
pub fn reference(scene: &Scene, gb: &[Px], tracer: &Tracer) -> Vec<V3> {
    let surf: Vec<usize> = (0..gb.len()).filter(|&k| matches!(gb[k], Px::Surf { .. })).collect();
    let reqs: Vec<Req> = surf
        .iter()
        .map(|&k| {
            let Px::Surf { p, n, albedo, .. } = gb[k] else { unreachable!() };
            Req { p, n, albedo, seed: mix64(k as u64 ^ 0xabcdef) }
        })
        .collect();
    let o = Opts { spp: REF_SPP as u32, ..Opts::plain() };
    let mut img: Vec<V3> = gb.iter().map(|px| if let Px::Light(e) = px { *e } else { V3::default() }).collect();
    for (k, l) in surf.iter().zip(tracer.trace(scene, &reqs, &o).l) {
        img[*k] = l;
    }
    img
}

/// (RMSE, relMSE) sur les pixels de surface.
pub fn errors(img: &[V3], reference: &[V3], gb: &[Px]) -> (f64, f64) {
    let (mut se, mut rel, mut cnt) = (0.0, 0.0, 0.0);
    for k in 0..img.len() {
        if let Px::Surf { .. } = gb[k] {
            for c in 0..3 {
                let (x, r) = (img[k].get(c), reference[k].get(c));
                se += (x - r) * (x - r);
                rel += (x - r) * (x - r) / (r * r + 0.01);
            }
            cnt += 3.0;
        }
    }
    ((se / cnt).sqrt(), rel / cnt)
}

pub fn save_raw(path: &str, img: &[V3]) {
    let mut b = Vec::with_capacity(img.len() * 12);
    for v in img {
        for c in [v.x, v.y, v.z] {
            b.extend_from_slice(&(c as f32).to_le_bytes());
        }
    }
    std::fs::write(path, b).expect("écriture référence");
}

pub fn load_raw(path: &str, len: usize) -> Option<Vec<V3>> {
    let b = std::fs::read(path).ok()?;
    if b.len() != len * 12 {
        return None;
    }
    let f = |i: usize| f32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap()) as f64;
    Some((0..len).map(|k| v3(f(3 * k), f(3 * k + 1), f(3 * k + 2))).collect())
}

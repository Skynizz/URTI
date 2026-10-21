//! Caméra, G-buffer, références et métriques.
use crate::math::{mix64, v3, Rng, V3};
use crate::par::par_map;
use crate::pt::{radiance, NoRec};
use crate::scene::Scene;

pub const W: usize = 192;
pub const H: usize = 96;
pub const REF_SPP: usize = 4096;

const EYE: V3 = v3(5.15, 1.7, 10.5);
const LOOK: V3 = v3(5.15, 1.2, 2.0);
const HFOV_DEG: f64 = 80.0;

#[derive(Clone, Copy)]
pub enum Px {
    Miss,
    Light(V3),
    Surf { p: V3, n: V3, albedo: V3, dist: f64 },
}

pub fn camera_ray(i: usize, j: usize) -> (V3, V3) {
    let f = (LOOK - EYE).norm();
    let r = f.cross(v3(0.0, 1.0, 0.0)).norm();
    let u = r.cross(f);
    let th = (HFOV_DEG.to_radians() * 0.5).tan();
    let tv = th * H as f64 / W as f64;
    let sx = 2.0 * (i as f64 + 0.5) / W as f64 - 1.0;
    let sy = 1.0 - 2.0 * (j as f64 + 0.5) / H as f64;
    (EYE, (f + r * (sx * th) + u * (sy * tv)).norm())
}

pub fn gbuffer(scene: &Scene) -> Vec<Px> {
    par_map(W * H, |k| {
        let (o, d) = camera_ray(k % W, k / W);
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
pub fn reference(scene: &Scene, gb: &[Px]) -> Vec<V3> {
    par_map(W * H, |k| match gb[k] {
        Px::Miss => V3::default(),
        Px::Light(e) => e,
        Px::Surf { p, n, albedo, .. } => {
            let mut rng = Rng::new(mix64(k as u64 ^ 0xabcdef));
            let mut acc = V3::default();
            for _ in 0..REF_SPP {
                acc += radiance(scene, p, n, albedo, &mut rng, &mut NoRec);
            }
            acc / REF_SPP as f64
        }
    })
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

pub fn load_raw(path: &str) -> Option<Vec<V3>> {
    let b = std::fs::read(path).ok()?;
    if b.len() != W * H * 12 {
        return None;
    }
    let f = |i: usize| f32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap()) as f64;
    Some((0..W * H).map(|k| v3(f(3 * k), f(3 * k + 1), f(3 * k + 2))).collect())
}

//! `krc bench` : validation du traceur GPU contre le CPU, puis débit (chemins/s).
use crate::gpu::Gpu;
use crate::math::{mix64, V3};
use crate::render::{gbuffer, Px, World};
use crate::scene::{build, State};
use crate::tracer::{Opts, Req, Tracer};
use std::time::Instant;

fn reqs_for(gb: &[Px], n: usize, salt: u64) -> Vec<Req> {
    let surf: Vec<&Px> = gb.iter().filter(|p| matches!(p, Px::Surf { .. })).collect();
    (0..n)
        .map(|i| {
            let Px::Surf { p, n, albedo, .. } = *surf[i % surf.len()] else { unreachable!() };
            Req { p, n, albedo, seed: mix64(i as u64 ^ salt) }
        })
        .collect()
}

/// relMSE entre deux estimations par pixel, et rapport des luminances moyennes.
fn compare(a: &[V3], b: &[V3]) -> (f64, f64) {
    let (mut rel, mut la, mut lb) = (0.0, 0.0, 0.0);
    for (x, y) in a.iter().zip(b) {
        for c in 0..3 {
            let d = x.get(c) - y.get(c);
            rel += d * d / (y.get(c) * y.get(c) + 0.01);
        }
        la += x.lum();
        lb += y.lum();
    }
    (rel / (3 * a.len()) as f64, la / lb)
}

fn timed<T>(f: impl FnOnce() -> T) -> (T, f64) {
    let t = Instant::now();
    let r = f();
    (r, t.elapsed().as_secs_f64() * 1e3)
}

pub fn bench(tracer: &Tracer) {
    let gpu = match tracer {
        Tracer::Gpu(_) => None,
        Tracer::Cpu => Gpu::new(),
    };
    let gpu_tracer;
    let g: &Tracer = match gpu {
        Some(x) => {
            gpu_tracer = Tracer::Gpu(x);
            &gpu_tracer
        }
        None if tracer.is_gpu() => tracer,
        None => {
            eprintln!("pas de GPU : banc impossible");
            return;
        }
    };
    let cpu = Tracer::Cpu;
    let world = World::new(2);
    let s0 = State { box_step: 0, l0_on: true, l2_on: false };
    let scene = build(&s0, 2);
    let gb = gbuffer(&scene, &world);
    let n_surf = gb.iter().filter(|p| matches!(p, Px::Surf { .. })).count();

    // 1. exactitude : 256 spp par pixel, GPU contre CPU, et CPU contre CPU (niveau de bruit)
    let o = Opts { spp: 256, ..Opts::plain() };
    let ra = reqs_for(&gb, n_surf, 1);
    let rb = reqs_for(&gb, n_surf, 2);
    let l = |t: &Tracer, r: &[Req]| t.trace(&scene, r, &o).l;
    let (c1, c2, g1) = (l(&cpu, &ra), l(&cpu, &rb), l(g, &ra));
    let (noise, _) = compare(&c1, &c2);
    let (diff, ratio) = compare(&g1, &c1);
    println!("exactitude (256 spp, {n_surf} px) : relMSE CPU/CPU = {noise:.3e} (bruit), GPU/CPU = {diff:.3e}, luminance GPU/CPU = {ratio:.4}");

    // 2. chemins corrélés sur GPU : L_ancien par le même chemin = retraçage dans l'ancienne scène
    let s1 = State { l2_on: true, ..s0 };
    let scene1 = build(&s1, 2);
    let w: Vec<f64> = (0..scene1.lights.len()).map(|j| if j == 2 { -1.0 } else { 0.0 }).collect();
    let r = reqs_for(&gb, 20_000, 3);
    let split = g.trace(&scene1, &r, &Opts { union: true, record: true, toggle_w: &w, spp: 1 });
    let full = g.trace(&scene, &r, &Opts { union: true, ..Opts::plain() });
    let max_err = (0..r.len()).map(|i| (split.l[i] + split.dl[i] - full.l[i]).len()).fold(0.0, f64::max);
    let scale = full.l.iter().map(|l| l.len()).fold(0.0, f64::max);
    println!("corrélation GPU : max |L+ΔL − L_ancien| = {max_err:.2e} (radiance max {scale:.2e})");

    // dépendances : statistiques agrégées GPU contre CPU (les chemins diffèrent, pas les lois)
    let rec = Opts { record: true, ..Opts::plain() };
    let r = reqs_for(&gb, 50_000, 4);
    let stat = |t: &Tracer| {
        let out = t.trace(&scene, &r, &rec).recs;
        let regions: f64 = out.iter().map(|o| o.regions.0.iter().map(|w| w.count_ones()).sum::<u32>() as f64).sum();
        let sups: f64 = out.iter().map(|o| o.vsup.count_ones() as f64).sum();
        let box0 = out.iter().filter(|o| o.objs & 1 != 0).count() as f64;
        let n = out.len() as f64;
        (regions / n, sups / n, box0 / n)
    };
    let (sc, sg) = (stat(&cpu), stat(g));
    println!(
        "dépendances (moyennes par chemin) CPU / GPU : régions {:.2} / {:.2}, super-régions {:.3} / {:.3}, touche la boîte {:.4} / {:.4}",
        sc.0, sg.0, sc.1, sg.1, sc.2, sg.2
    );

    // 2 bis. où passe le temps : calcul (spp élevé) ou surcoût fixe (transferts, allocations)
    let r = reqs_for(&gb, 262_144, 6);
    for spp in [1u32, 4, 16, 64] {
        let o = Opts { spp, ..Opts::plain() };
        let (_, tg) = timed(|| g.trace(&scene, &r, &o));
        println!("  GPU 262144 requêtes × {spp:>2} spp : {tg:7.1} ms ({:7.1} M chemins/s)", 262_144.0 * spp as f64 / tg / 1e3);
    }

    // 2 ter. profil d'un lot 1080p : où passent les millisecondes
    if let Tracer::Gpu(gg) = g {
        let r = reqs_for(&gb, 1920 * 1080, 7);
        for record in [false, true] {
            let o = Opts { record, ..Opts::plain() };
            let _ = g.trace(&scene, &r, &o);
            *gg.prof.lock().unwrap() = Default::default();
            let (_, total) = timed(|| g.trace(&scene, &r, &o));
            let p = gg.prof.lock().unwrap().clone();
            println!(
                "profil 1080p, dépendances {:<3} : total {total:6.1} ms = préparation {:5.1} + envoi {:5.1} + attente GPU {:5.1} (dont noyau {:5.1}) + relecture {:5.1} + conversion {:5.1} + hors lot {:5.1}",
                if record { "oui" } else { "non" },
                p.build,
                p.upload,
                p.wait,
                p.kernel,
                p.read,
                p.convert,
                total - p.total()
            );
        }
    }

    // 3. débit
    println!("débit (scène 2 pièces, 1 chemin par requête, transferts compris) :");
    for &n in &[n_surf, 262_144, 1920 * 1080] {
        let r = reqs_for(&gb, n, 5);
        for record in [false, true] {
            let o = Opts { record, ..Opts::plain() };
            let _ = g.trace(&scene, &r[..r.len().min(4096)], &o); // préchauffage
            let (_, tg) = timed(|| g.trace(&scene, &r, &o));
            let (_, tc) = timed(|| cpu.trace(&scene, &r, &o));
            println!(
                "  {n:>8} chemins, dépendances {:<3} : CPU {tc:8.1} ms ({:6.1} M/s) | GPU {tg:8.1} ms ({:6.1} M/s) | ×{:.1}",
                if record { "oui" } else { "non" },
                n as f64 / tc / 1e3,
                n as f64 / tg / 1e3,
                tc / tg
            );
        }
    }
}

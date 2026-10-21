mod cache;
mod deps;
mod image;
mod math;
mod methods;
mod par;
mod pt;
mod render;
mod rt;
mod bench;
mod gpu;
mod gpu_krc;
mod grid;
mod scene;
mod tracer;

use render::{gbuffer, load_raw, reference, save_raw, World};
use scene::{build, state_at, FRAMES};
use std::collections::HashMap;
use std::time::Instant;

fn ensure_refs(world: &World, dir: &str, tracer: &tracer::Tracer) -> HashMap<String, Vec<math::V3>> {
    std::fs::create_dir_all(dir).unwrap();
    let mut refs = HashMap::new();
    for f in 0..FRAMES {
        let s = state_at(f);
        let id = s.id();
        if refs.contains_key(&id) {
            continue;
        }
        let path = format!("{dir}/{id}.bin");
        let img = load_raw(&path, world.len()).unwrap_or_else(|| {
            let t = Instant::now();
            let scene = build(&s, world.rooms);
            let img = reference(&scene, &gbuffer(&scene, world), tracer);
            save_raw(&path, &img);
            image::save(&format!("{dir}/{id}.png"), world.w, world.h, &img);
            eprintln!("référence {dir}/{id} : {:.1} s", t.elapsed().as_secs_f64());
            img
        });
        refs.insert(id, img);
    }
    refs
}

/// Phases de la séquence : (nom, première frame, dernière frame exclue).
const PHASES: [(&str, usize, usize); 6] = [
    ("tout", 0, 120),
    ("statique", 30, 40),
    ("boite", 40, 64),
    ("apres_boite", 64, 80),
    ("lampe_on", 80, 100),
    ("lampe_off", 100, 120),
];

fn mean_std(v: &[f64]) -> (f64, f64) {
    let n = v.len() as f64;
    let m = v.iter().sum::<f64>() / n;
    let var = if v.len() > 1 { v.iter().map(|x| (x - m) * (x - m)).sum::<f64>() / (n - 1.0) } else { 0.0 };
    (m, var.sqrt())
}

fn run(
    names: &[String],
    seeds: u64,
    refs: &HashMap<String, Vec<math::V3>>,
    world: &World,
    root: &str,
    warmup: u32,
    tracer: &tracer::Tracer,
) -> String {
    let mut all = methods::all_methods();
    if !names.is_empty() {
        all.retain(|c| names.contains(&c.name));
    }
    if !tracer.is_gpu() {
        all.retain(|c| !c.name.starts_with("gpu_"));
    }
    let mut summary = String::from("method,phase,relmse_mean,relmse_std,rmse_mean,paths_per_frame\n");
    for cfg in &all {
        let t = Instant::now();
        let dir = format!("{root}/{}", cfg.name);
        let mut runs = Vec::new();
        for seed in 0..seeds {
            let stats = methods::run(cfg, refs, &dir, seed, world, warmup, tracer);
            let mut csv = String::from(methods::FrameStats::HEADER);
            csv.push('\n');
            for s in &stats {
                csv += &s.csv();
                csv.push('\n');
            }
            let name = if seed == 0 { "metrics.csv".to_string() } else { format!("metrics_s{seed}.csv") };
            std::fs::write(format!("{dir}/{name}"), csv).unwrap();
            runs.push(stats);
        }
        let mut line = format!("{:<18}", cfg.name);
        for (phase, a, b) in PHASES {
            let per_run = |f: &dyn Fn(&methods::FrameStats) -> f64| -> Vec<f64> {
                runs.iter().map(|r| r[a..b].iter().map(f).sum::<f64>() / (b - a) as f64).collect()
            };
            let (rel_m, rel_s) = mean_std(&per_run(&|s| s.relmse));
            let (rmse_m, _) = mean_std(&per_run(&|s| s.rmse));
            let (paths_m, _) = mean_std(&per_run(&|s| s.paths as f64));
            summary += &format!("{},{phase},{rel_m:.6e},{rel_s:.6e},{rmse_m:.6e},{paths_m:.1}\n", cfg.name);
            line += &format!(" | {phase} {rel_m:.2e}±{rel_s:.1e}");
        }
        eprintln!("{line} | {:.1} s", t.elapsed().as_secs_f64());
    }
    let path = if names.is_empty() { format!("{root}/summary.csv") } else { format!("{root}/summary_partial.csv") };
    std::fs::write(path, &summary).unwrap();
    summary
}

/// Passage à l'échelle : N pièces, mêmes événements (pièces 0 et 1), densité de pixels constante.
fn scale(tracer: &tracer::Tracer) {
    let only: Option<Vec<String>> = std::env::var("KRC_METHODS").ok().map(|v| v.split(',').map(String::from).collect());
    let names: Vec<String> = [
        "cache_reset",
        "krc_hard",
        "krc_all",
        "krc_noage",
        "krc",
        "cache_reset_target",
        "krc_hard_target",
        "krc_all_target",
        "krc_noage_target",
        "krc_target",
        "krc_energy",
        "krc_energy_target",
        "krc_energy_l1",
        "krc_energy_l2",
        "krc_energy_l2_target",
    ]
        .iter()
        .map(|s| s.to_string())
        .filter(|s| only.as_ref().map_or(true, |o| o.contains(s)))
        .collect();
    let mut all = String::from("rooms,method,phase,relmse_mean,relmse_std,rmse_mean,paths_per_frame\n");
    let rooms_list: Vec<usize> = std::env::var("KRC_ROOMS")
        .map(|v| v.split(',').filter_map(|x| x.parse().ok()).collect())
        .unwrap_or_else(|_| vec![2, 4, 8]);
    for rooms in rooms_list {
        let world = World::new(rooms);
        let refs = ensure_refs(&world, &format!("results/ref_n{rooms}"), tracer);
        eprintln!("--- {rooms} pièces");
        let summary = run(&names, seeds(), &refs, &world, &format!("{}/scale/n{rooms}", out_root(tracer)), warmup(), tracer);
        for line in summary.lines().skip(1) {
            all += &format!("{rooms},{line}\n");
        }
    }
    if only.is_none() {
        std::fs::write(format!("{}/scale/summary.csv", out_root(tracer)), all).unwrap();
    }
}

/// Nombre de graines indépendantes par méthode (variable d'environnement KRC_SEEDS, 5 par défaut).
fn seeds() -> u64 {
    std::env::var("KRC_SEEDS").ok().and_then(|v| v.parse().ok()).unwrap_or(5)
}

/// Frames de préchauffage avant la séquence de passage à l'échelle (KRC_WARMUP, 150 par défaut) :
/// les coûts mesurés sont alors ceux des événements, pas de la convergence initiale.
fn warmup() -> u32 {
    std::env::var("KRC_WARMUP").ok().and_then(|v| v.parse().ok()).unwrap_or(150)
}

/// Dossier des résultats : `results` (CPU), `results/gpu` (GPU), ou KRC_OUT.
fn out_root(tracer: &tracer::Tracer) -> String {
    std::env::var("KRC_OUT").unwrap_or_else(|_| if tracer.is_gpu() { "results/gpu".into() } else { "results".into() })
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let tracer = tracer::Tracer::from_env();
    let w2 = World::new(2);
    match args.first().map(String::as_str) {
        Some("refs") => {
            ensure_refs(&w2, "results/ref", &tracer);
        }
        Some("run") => {
            run(&args[1..], seeds(), &ensure_refs(&w2, "results/ref", &tracer), &w2, &out_root(&tracer), 0, &tracer);
        }
        Some("all") => {
            run(&[], seeds(), &ensure_refs(&w2, "results/ref", &tracer), &w2, &out_root(&tracer), 0, &tracer);
        }
        Some("scale") => scale(&tracer),
        Some("bench") => bench::bench(&tracer),
        Some("drift") => match gpu::Gpu::new() {
            Some(g) => {
                let cycles = args.get(1).and_then(|c| c.parse().ok()).unwrap_or(100);
                gpu_krc::drift(&g, cycles, &ensure_refs(&w2, "results/ref", &tracer));
            }
            None => eprintln!("pas de GPU"),
        },
        Some("cornell") => match gpu::Gpu::new() {
            Some(g) => {
                let size = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(256);
                rt::run_cornell(&g, seeds(), size);
            }
            None => eprintln!("pas de GPU"),
        },
        Some("rtsmoke") => match gpu::Gpu::new() {
            Some(g) => rt::smoke(&g),
            None => eprintln!("pas de GPU"),
        },
        Some("gpubench") => match gpu::Gpu::new() {
            Some(g) => gpu_krc::bench(&g),
            None => eprintln!("pas de GPU"),
        },
        _ => eprintln!("usage : krc refs | run [méthodes...] | all | scale | bench"),
    }
}

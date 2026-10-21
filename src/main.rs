mod cache;
mod deps;
mod image;
mod math;
mod methods;
mod par;
mod pt;
mod render;
mod scene;

use render::{gbuffer, load_raw, reference, save_raw, H, W};
use scene::{build, state_at, FRAMES};
use std::collections::HashMap;
use std::time::Instant;

fn ensure_refs() -> HashMap<String, Vec<math::V3>> {
    std::fs::create_dir_all("results/ref").unwrap();
    let mut refs = HashMap::new();
    for f in 0..FRAMES {
        let s = state_at(f);
        let id = s.id();
        if refs.contains_key(&id) {
            continue;
        }
        let path = format!("results/ref/{id}.bin");
        let img = load_raw(&path).unwrap_or_else(|| {
            let t = Instant::now();
            let scene = build(&s);
            let img = reference(&scene, &gbuffer(&scene));
            save_raw(&path, &img);
            image::save(&format!("results/ref/{id}.png"), W, H, &img);
            eprintln!("référence {id} : {:.1} s", t.elapsed().as_secs_f64());
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

fn run(names: &[String], seeds: u64, refs: &HashMap<String, Vec<math::V3>>) {
    let mut all = methods::all_methods();
    if !names.is_empty() {
        all.retain(|c| names.contains(&c.name));
    }
    let mut summary = String::from("method,phase,relmse_mean,relmse_std,rmse_mean,paths_per_frame\n");
    for cfg in &all {
        let t = Instant::now();
        let dir = format!("results/{}", cfg.name);
        let mut runs = Vec::new();
        for seed in 0..seeds {
            let stats = methods::run(cfg, refs, &dir, seed);
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
    let path = if names.is_empty() { "results/summary.csv".to_string() } else { "results/summary_partial.csv".to_string() };
    std::fs::write(path, summary).unwrap();
}

/// Nombre de graines indépendantes par méthode (variable d'environnement KRC_SEEDS, 5 par défaut).
fn seeds() -> u64 {
    std::env::var("KRC_SEEDS").ok().and_then(|v| v.parse().ok()).unwrap_or(5)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("refs") => {
            ensure_refs();
        }
        Some("run") => run(&args[1..], seeds(), &ensure_refs()),
        Some("all") => run(&[], seeds(), &ensure_refs()),
        _ => eprintln!("usage : krc refs | run [méthodes...] | all"),
    }
}

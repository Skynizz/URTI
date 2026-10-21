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

fn run(names: &[String], refs: &HashMap<String, Vec<math::V3>>) {
    let mut all = methods::all_methods();
    if !names.is_empty() {
        all.retain(|c| names.contains(&c.name));
    }
    for cfg in &all {
        let t = Instant::now();
        let dir = format!("results/{}", cfg.name);
        let stats = methods::run(cfg, refs, &dir);
        let mut csv = String::from(methods::FrameStats::HEADER);
        csv.push('\n');
        for s in &stats {
            csv += &s.csv();
            csv.push('\n');
        }
        std::fs::write(format!("{dir}/metrics.csv"), csv).unwrap();
        let mean = |a: usize, b: usize| stats[a..b].iter().map(|s| s.relmse).sum::<f64>() / (b - a) as f64;
        let paths: usize = stats.iter().map(|s| s.paths).sum();
        eprintln!(
            "{:<18} relMSE moy {:.4e} | statique {:.4e} | boîte {:.4e} | lampe on {:.4e} | lampe off {:.4e} | chemins {} | {:.1} s",
            cfg.name,
            mean(0, 120),
            mean(30, 40),
            mean(40, 64),
            mean(80, 100),
            mean(100, 120),
            paths,
            t.elapsed().as_secs_f64()
        );
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("refs") => {
            ensure_refs();
        }
        Some("run") => run(&args[1..], &ensure_refs()),
        Some("all") => run(&[], &ensure_refs()),
        _ => eprintln!("usage : krc refs | run [méthodes...] | all"),
    }
}

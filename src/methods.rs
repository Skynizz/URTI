//! Les méthodes comparées et la boucle de séquence.
use crate::cache::{cell_key, zscore, Cache, Stat};
use crate::deps::{mark_aabb, mark_point, Bits};
use crate::math::{mix64, v3, Rng, V3};
use crate::par::par_map;
use crate::pt::{radiance, DepRec};
use crate::render::{errors, gbuffer, Px, H, W};
use crate::scene::{build, cosine_dir, moving_box_aabb, state_at, Scene, State, FRAMES, LIGHT_ID_BASE, MOVING_BOX};
use std::collections::{BinaryHeap, HashMap};

pub const K: usize = 4; // clés jitterées par pixel
const N_CAP: f64 = 64.0; // poids max de l'historique d'une cellule suspecte
const N_VERIFY: f64 = 8.0; // échantillons frais avant de confirmer
const Z_VERIFY: f64 = 3.0;
const SAFETY_WINDOW: f64 = 32.0;
const Z_SAFETY: f64 = 4.0;
const EXPLORE_PERIOD: i64 = 16;
const KAPPA: f64 = 4.0; // poids de l'a priori bayésien sur la variance
const PRIOR_FLOOR: f64 = 0.1;
const REL_EPS: f64 = 0.01; // même ε que relMSE
const EMA_CACHE: f64 = 1.0 / 64.0;
const EMA_PIXEL: f64 = 0.1;
const PROBES: usize = 1024;
const TARGET_CAP: usize = 4; // en mode qualité cible : au plus 4 chemins/pixel/frame
const TARGET_PER_CELL: f64 = 32.0;

#[derive(Clone, Copy, PartialEq)]
pub enum OnEvent {
    Nothing,
    GlobalReset,
    Invalidate { spatial: bool, verify: bool },
}

#[derive(Clone, Copy)]
pub enum Kind {
    PixelReset,
    PixelEma,
    Cache { neyman: bool, ema: bool, on_event: OnEvent, safety: bool },
}

#[derive(Clone)]
pub struct Cfg {
    pub name: String,
    pub kind: Kind,
    /// Mode qualité cible : erreur relative visée par cellule (sinon budget fixe).
    pub target: Option<f64>,
}

fn cache(name: &str, neyman: bool, ema: bool, on_event: OnEvent, safety: bool) -> Cfg {
    Cfg { name: name.into(), kind: Kind::Cache { neyman, ema, on_event, safety }, target: None }
}

pub fn all_methods() -> Vec<Cfg> {
    use OnEvent::*;
    let mut v = vec![
        Cfg { name: "pt_reset".into(), kind: Kind::PixelReset, target: None },
        Cfg { name: "pt_ema".into(), kind: Kind::PixelEma, target: None },
        cache("cache_ema", false, true, Nothing, false),
        cache("cache_reset", true, false, GlobalReset, false),
        cache("krc_nodep", true, false, Nothing, true),
        cache("krc_obj", true, false, Invalidate { spatial: false, verify: true }, true),
        cache("krc_hard", true, false, Invalidate { spatial: true, verify: false }, true),
        cache("krc", true, false, Invalidate { spatial: true, verify: true }, true),
    ];
    for base in ["krc", "cache_reset"] {
        let mut c = v.iter().find(|c| c.name == base).unwrap().clone();
        c.name = format!("{base}_target");
        c.target = Some(0.03);
        v.push(c);
    }
    v
}

/// Description d'un changement de scène.
pub struct Event {
    pub regions: Bits,
    pub objs: u64,
    pub probe_rays: usize,
}

/// Rayons-sondes depuis une lampe : régions de leurs premiers impacts.
fn light_probes(scene: &Scene, j: usize, seed: u64) -> Bits {
    let l = &scene.lights[j];
    let mut bits = Bits::default();
    mark_aabb(&mut bits, &l.aabb());
    let mut rng = Rng::new(seed);
    for _ in 0..PROBES {
        let o = l.center + v3((2.0 * rng.f64() - 1.0) * l.hx, -1e-4, (2.0 * rng.f64() - 1.0) * l.hz);
        let d = cosine_dir(v3(0.0, -1.0, 0.0), &mut rng);
        if let Some(h) = scene.intersect(o, d, f64::INFINITY) {
            mark_point(&mut bits, h.p);
        }
    }
    bits
}

pub fn events(prev: &State, cur: &State, prev_scene: &Scene, cur_scene: &Scene, frame: u32) -> Vec<Event> {
    let mut ev = Vec::new();
    if prev.box_step != cur.box_step {
        let swept = moving_box_aabb(prev.box_step).union(moving_box_aabb(cur.box_step)).expand(0.05);
        let mut regions = Bits::default();
        mark_aabb(&mut regions, &swept);
        ev.push(Event { regions, objs: 1 << MOVING_BOX, probe_rays: 0 });
    }
    for (j, was, is) in [(0, prev.l0_on, cur.l0_on), (2, prev.l2_on, cur.l2_on)] {
        if was != is {
            // on sonde depuis l'état où la lampe est allumée
            let scene = if is { cur_scene } else { prev_scene };
            let regions = light_probes(scene, j, mix64(frame as u64 * 31 + j as u64));
            ev.push(Event { regions, objs: 1 << (LIGHT_ID_BASE + j), probe_rays: PROBES });
        }
    }
    ev
}

#[derive(Default, Clone)]
pub struct FrameStats {
    pub frame: u32,
    pub paths: usize,
    pub paths_a: usize,
    pub paths_b: usize,
    pub probes: usize,
    pub suspects: usize,
    pub hard_resets: usize,
    pub rejected: usize,
    pub confirmed: usize,
    pub safety: usize,
    pub cells: usize,
    pub rmse: f64,
    pub relmse: f64,
}

impl FrameStats {
    pub const HEADER: &'static str =
        "frame,paths,paths_a,paths_b,probes,suspects,hard_resets,rejected,confirmed,safety,cells,rmse,relmse";
    pub fn csv(&self) -> String {
        format!(
            "{},{},{},{},{},{},{},{},{},{},{},{:.6e},{:.6e}",
            self.frame, self.paths, self.paths_a, self.paths_b, self.probes, self.suspects, self.hard_resets,
            self.rejected, self.confirmed, self.safety, self.cells, self.rmse, self.relmse
        )
    }
}

pub const SAVE_FRAMES: [u32; 10] = [39, 41, 63, 79, 80, 81, 99, 100, 101, 119];

fn post_var(n: f64, rel: f64, prior: f64) -> f64 {
    if n < 1.0 { prior } else { (KAPPA * prior + (n - 1.0) * rel) / (KAPPA + n - 1.0) }
}

/// (n effectif, variance relative brute) d'une cellule, en tenant compte de l'état suspect.
fn uncertainty(c: &crate::cache::Cell) -> (f64, f64) {
    let (n, var) = match &c.fresh {
        Some(f) => (f.n, c.main.var().max(f.var())),
        None => (c.main.n, c.main.var()),
    };
    let m = c.main.lmean.max(c.fresh.map_or(0.0, |f| f.lmean));
    (n, var / (m * m + REL_EPS))
}

fn task_rng(seed: u64, frame: u32, i: usize) -> Rng {
    Rng::new(mix64(seed ^ mix64((frame as u64) << 32 | i as u64)))
}

pub fn run(cfg: &Cfg, refs: &HashMap<String, Vec<V3>>, out_dir: &str) -> Vec<FrameStats> {
    std::fs::create_dir_all(out_dir).unwrap();
    let seed = mix64(cfg.name.bytes().fold(7u64, |h, b| h.wrapping_mul(131) ^ b as u64));
    let mut stats = Vec::new();
    let mut prev_state = state_at(0);
    let mut prev_scene = build(&prev_state);
    let mut pixels: Vec<Stat> = vec![Stat::default(); W * H];
    let mut cache = Cache::default();
    let mut rng = Rng::new(seed);

    for frame in 0..FRAMES {
        let state = state_at(frame);
        let scene = build(&state);
        let mut fs = FrameStats { frame, ..Default::default() };
        let evs = if frame > 0 && state != prev_state {
            events(&prev_state, &state, &prev_scene, &scene, frame)
        } else {
            Vec::new()
        };
        let gb = gbuffer(&scene);
        let surf: Vec<usize> = (0..W * H).filter(|&k| matches!(gb[k], Px::Surf { .. })).collect();
        let budget = surf.len();
        let mut img = vec![V3::default(); W * H];
        for k in 0..W * H {
            if let Px::Light(e) = gb[k] {
                img[k] = e;
            }
        }

        match cfg.kind {
            Kind::PixelReset | Kind::PixelEma => {
                if matches!(cfg.kind, Kind::PixelReset) && !evs.is_empty() {
                    pixels.iter_mut().for_each(|s| *s = Stat::default());
                }
                let samples = par_map(surf.len(), |i| {
                    let Px::Surf { p, n, albedo, .. } = gb[surf[i]] else { unreachable!() };
                    let mut r = task_rng(seed, frame, i);
                    radiance(&scene, p, n, albedo, &mut r, &mut crate::pt::NoRec)
                });
                for (i, v) in samples.into_iter().enumerate() {
                    let s = &mut pixels[surf[i]];
                    match cfg.kind {
                        Kind::PixelReset => s.add(v),
                        _ => s.add_ema(v, EMA_PIXEL),
                    }
                    if gb[surf[i]].is_surf_a() {
                        fs.paths_a += 1;
                    } else {
                        fs.paths_b += 1;
                    }
                }
                for &k in &surf {
                    img[k] = pixels[k].mean;
                }
                fs.paths = surf.len();
            }
            Kind::Cache { neyman, ema, on_event, safety } => {
                // 1. événements
                for e in &evs {
                    fs.probes += e.probe_rays;
                    for c in cache.cells.iter_mut() {
                        match on_event {
                            OnEvent::Nothing => {}
                            OnEvent::GlobalReset => c.clear(),
                            OnEvent::Invalidate { spatial, verify } => {
                                let hit = if spatial { c.deps.intersects(&e.regions) } else { c.objs & e.objs != 0 };
                                if !hit || c.main.n == 0.0 {
                                    continue;
                                }
                                if verify {
                                    c.main.cap(N_CAP);
                                    c.fresh = Some(Stat::default());
                                    c.recent = Stat::default();
                                    fs.suspects += 1;
                                } else {
                                    c.clear();
                                    fs.hard_resets += 1;
                                }
                            }
                        }
                    }
                }
                // 2. cellules visibles
                let mut pix_cells = vec![[u32::MAX; K]; W * H];
                for &k in &surf {
                    let Px::Surf { p, n, dist, .. } = gb[k] else { unreachable!() };
                    for s in 0..K {
                        pix_cells[k][s] = cache.get_or_create(cell_key(p, n, dist, k, s), p.x);
                    }
                }
                let nc = cache.cells.len();
                let mut foot = vec![0.0f64; nc];
                let mut refs: Vec<Vec<u32>> = vec![Vec::new(); nc];
                for &k in &surf {
                    for s in 0..K {
                        let c = pix_cells[k][s] as usize;
                        foot[c] += 1.0 / K as f64;
                        refs[c].push(k as u32);
                    }
                }
                let visible: Vec<usize> = (0..nc).filter(|&c| foot[c] > 0.0).collect();
                fs.cells = visible.len();
                // 3. allocation
                let mut tasks: Vec<(u32, u32)> = Vec::new();
                let push = |c: usize, tasks: &mut Vec<(u32, u32)>, rng: &mut Rng| {
                    let r = &refs[c];
                    tasks.push((c as u32, r[rng.below(r.len())]));
                };
                if !neyman {
                    for &k in &surf {
                        let c = pix_cells[k][rng.below(K)];
                        tasks.push((c, k as u32));
                    }
                } else {
                    // a priori : médiane sur toutes les cellules mûres (suspectes comprises), avec plancher,
                    // sinon les seules cellules restantes (p. ex. faces tournées vers le vide, σ = 0) l'effondrent
                    let mut rels: Vec<f64> = visible
                        .iter()
                        .map(|&c| &cache.cells[c].main)
                        .filter(|m| m.n >= 8.0)
                        .map(|m| m.var() / (m.lmean * m.lmean + REL_EPS))
                        .collect();
                    let prior = if rels.len() < 16 {
                        1.0
                    } else {
                        let m = rels.len() / 2;
                        rels.select_nth_unstable_by(m, |a, b| a.total_cmp(b)).1.max(PRIOR_FLOOR)
                    };
                    let cap = match cfg.target {
                        Some(_) => budget * TARGET_CAP,
                        None => budget,
                    };
                    // plancher d'exploration
                    for &c in &visible {
                        if frame as i64 - cache.cells[c].last >= EXPLORE_PERIOD && cache.cells[c].main.n > 0.0 {
                            push(c, &mut tasks, &mut rng);
                        }
                    }
                    let mut extra = vec![0f64; nc];
                    let mut heap = BinaryHeap::new();
                    let mut need_total = 0.0;
                    let mut needs = Vec::new();
                    for &c in &visible {
                        let (n, rel) = uncertainty(&cache.cells[c]);
                        let pv = post_var(n, rel, prior);
                        if let Some(tau) = cfg.target {
                            let mut need = (pv / (tau * tau) - n).ceil().clamp(0.0, TARGET_PER_CELL);
                            if cache.cells[c].fresh.is_some() {
                                need = need.max(N_VERIFY - n);
                            }
                            if n == 0.0 {
                                need = need.max(4.0);
                            }
                            need_total += need;
                            needs.push((c, need));
                        }
                        heap.push((gain(foot[c], pv, n).to_bits(), c));
                    }
                    let remaining = cap.saturating_sub(tasks.len());
                    if cfg.target.is_some() && need_total <= remaining as f64 {
                        for (c, need) in needs {
                            for _ in 0..need as usize {
                                push(c, &mut tasks, &mut rng);
                            }
                        }
                    } else {
                        let mut left = remaining;
                        while left > 0 {
                            let Some((_, c)) = heap.pop() else { break };
                            push(c, &mut tasks, &mut rng);
                            extra[c] += 1.0;
                            let (n, rel) = uncertainty(&cache.cells[c]);
                            heap.push((gain(foot[c], post_var(n, rel, prior), n + extra[c]).to_bits(), c));
                            left -= 1;
                        }
                    }
                }
                // 4. tracé (parallèle)
                let results = par_map(tasks.len(), |i| {
                    let Px::Surf { p, n, albedo, .. } = gb[tasks[i].1 as usize] else { unreachable!() };
                    let mut r = task_rng(seed, frame, i);
                    let mut rec = DepRec::default();
                    let v = radiance(&scene, p, n, albedo, &mut r, &mut rec);
                    (v, rec)
                });
                // 5. mise à jour
                let mut touched = Vec::new();
                for (i, (v, rec)) in results.iter().enumerate() {
                    let ci = tasks[i].0 as usize;
                    let c = &mut cache.cells[ci];
                    if c.last != frame as i64 {
                        touched.push(ci);
                    }
                    c.last = frame as i64;
                    c.deps.or(&rec.regions);
                    c.objs |= rec.objs;
                    if c.cx < 5.2 {
                        fs.paths_a += 1;
                    } else {
                        fs.paths_b += 1;
                    }
                    if ema {
                        c.main.add_ema(*v, EMA_CACHE);
                    } else if let Some(f) = c.fresh.as_mut() {
                        f.add(*v);
                    } else {
                        c.main.add(*v);
                        if safety {
                            c.recent.add(*v);
                        }
                    }
                }
                fs.paths = tasks.len() + fs.probes;
                // 6. vérification des suspects et filet de sécurité
                for ci in touched {
                    let c = &mut cache.cells[ci];
                    if let Some(f) = c.fresh {
                        if f.n >= 2.0 && zscore(&f, &c.main) > Z_VERIFY {
                            c.main = f;
                            c.fresh = None;
                            fs.rejected += 1;
                        } else if f.n >= N_VERIFY {
                            c.main.merge(&f);
                            c.fresh = None;
                            fs.confirmed += 1;
                        }
                    } else if safety && c.recent.n >= SAFETY_WINDOW {
                        if c.main.n > c.recent.n && zscore(&c.recent, &c.main) > Z_SAFETY {
                            c.main = c.recent;
                            fs.safety += 1;
                        }
                        c.recent = Stat::default();
                    }
                }
                // 7. affichage
                for &k in &surf {
                    let (mut acc, mut w) = (V3::default(), 0.0);
                    for s in 0..K {
                        if let Some(v) = cache.cells[pix_cells[k][s] as usize].display() {
                            acc += v;
                            w += 1.0;
                        }
                    }
                    if w > 0.0 {
                        img[k] = acc / w;
                    }
                }
            }
        }

        let (rmse, relmse) = errors(&img, &refs[&state.id()], &gb);
        fs.rmse = rmse;
        fs.relmse = relmse;
        if SAVE_FRAMES.contains(&frame) {
            crate::image::save(&format!("{out_dir}/f{frame:03}.png"), W, H, &img);
        }
        stats.push(fs);
        prev_state = state;
        prev_scene = scene;
    }
    stats
}

/// Gain marginal (réduction d'erreur image attendue) d'un échantillon de plus.
fn gain(foot: f64, pv: f64, n: f64) -> f64 {
    if n < 1.0 { foot * 1e6 } else { foot * pv / (n * (n + 1.0)) }
}

impl Px {
    fn is_surf_a(&self) -> bool {
        matches!(self, Px::Surf { p, .. } if p.x < 5.2)
    }
}

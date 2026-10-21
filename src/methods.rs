//! Les méthodes comparées et la boucle de séquence.
use crate::cache::{cell_key, zscore, Cache, Cell, Stat};
use crate::deps::{mark_aabb, mark_point, Bits};
use crate::math::{mix64, v3, Rng, V3};
use crate::par::par_map;
use crate::pt::{radiance, radiance_split, DepRec, NoRec};
use crate::render::{errors, gbuffer, Px, World};
use crate::scene::{build, cosine_dir, moving_box_aabb, state_at, Scene, State, FRAMES, LIGHT_ID_BASE, MOVING_BOX};
use std::collections::{BinaryHeap, HashMap};

pub const K: usize = 4; // clés jitterées par pixel
const N_CAP: f64 = 64.0; // z-test : poids max de l'historique d'une cellule suspecte
const N_VERIFY: f64 = 8.0; // z-test : échantillons frais avant de confirmer
const Z_VERIFY: f64 = 3.0;
const M_DELTA0: usize = 2; // chemins corrélés par cellule suspecte, 1er tour
const M_DELTA: usize = 8; // total si l'un des chemins du 1er tour est affecté
const DEBT_RATE: f64 = 0.5; // part max du budget d'une frame servant à rembourser les chemins différentiels
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

/// Qui est suspect après un changement.
#[derive(Clone, Copy, PartialEq)]
pub enum Deps {
    /// régions traversées (H*)
    Spatial,
    /// objets touchés (H0)
    Objects,
    /// toutes les cellules (pas de graphe)
    All,
}

/// Que fait-on d'une cellule suspecte.
#[derive(Clone, Copy, PartialEq)]
pub enum Action {
    /// on jette l'historique
    Hard,
    /// échantillons frais + z-test contre l'historique
    ZTest,
    /// chemins corrélés ancienne/nouvelle scène ; `cv` : variable de contrôle à β optimal,
    /// sinon β = 1 (moyenne += Δ̂)
    Delta { cv: bool },
}

#[derive(Clone, Copy, PartialEq)]
pub enum OnEvent {
    Nothing,
    GlobalReset,
    Invalidate(Deps, Action),
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
    use Action::*;
    use Deps::*;
    use OnEvent::*;
    let mut v = vec![
        Cfg { name: "pt_reset".into(), kind: Kind::PixelReset, target: None },
        Cfg { name: "pt_ema".into(), kind: Kind::PixelEma, target: None },
        cache("cache_ema", false, true, Nothing, false),
        cache("cache_reset", true, false, GlobalReset, false),
        cache("krc_nodep", true, false, Nothing, true),
        cache("krc_obj", true, false, Invalidate(Objects, Delta { cv: true }), true),
        cache("krc_all", true, false, Invalidate(All, Delta { cv: true }), true),
        cache("krc_hard", true, false, Invalidate(Spatial, Hard), true),
        cache("krc_ztest", true, false, Invalidate(Spatial, ZTest), true),
        cache("krc_delta", true, false, Invalidate(Spatial, Delta { cv: false }), true),
        cache("krc", true, false, Invalidate(Spatial, Delta { cv: true }), true),
    ];
    for base in ["krc", "krc_all", "krc_hard", "cache_reset"] {
        let mut c = v.iter().find(|c| c.name == base).unwrap().clone();
        c.name = format!("{base}_target");
        c.target = Some(0.03);
        v.push(c);
    }
    v
}

/// Description d'un changement de scène (tous les changements d'une frame, fusionnés).
#[derive(Default)]
pub struct Event {
    pub regions: Bits,
    pub objs: u128,
    /// lampes dont l'état a changé, et poids allumée_avant − allumée_après
    pub lights: u64,
    pub light_w: Vec<f64>,
    /// un objet a bougé (sinon seules des lampes ont changé)
    pub geometry: bool,
    pub probe_rays: usize,
}

/// Rayons-sondes depuis une lampe : régions de leurs premiers impacts.
fn light_probes(scene: &Scene, j: usize, seed: u64, bits: &mut Bits) {
    let l = &scene.lights[j];
    mark_aabb(bits, &l.aabb());
    let mut rng = Rng::new(seed);
    for _ in 0..PROBES {
        let o = l.center + v3((2.0 * rng.f64() - 1.0) * l.hx, -1e-4, (2.0 * rng.f64() - 1.0) * l.hz);
        let d = cosine_dir(v3(0.0, -1.0, 0.0), &mut rng);
        if let Some(h) = scene.intersect(o, d, f64::INFINITY) {
            mark_point(bits, h.p);
        }
    }
}

pub fn event(prev: &State, cur: &State, prev_scene: &Scene, cur_scene: &Scene, frame: u32) -> Option<Event> {
    if prev == cur {
        return None;
    }
    let mut ev = Event::default();
    if prev.box_step != cur.box_step {
        let swept = moving_box_aabb(prev.box_step).union(moving_box_aabb(cur.box_step)).expand(0.05);
        mark_aabb(&mut ev.regions, &swept);
        ev.objs |= 1u128 << MOVING_BOX;
        ev.geometry = true;
    }
    ev.light_w = vec![0.0; cur_scene.lights.len()];
    for (j, was, is) in [(0, prev.l0_on, cur.l0_on), (2, prev.l2_on, cur.l2_on)] {
        if was != is {
            ev.light_w[j] = was as u8 as f64 - is as u8 as f64;
            // on sonde depuis l'état où la lampe est allumée
            let scene = if is { cur_scene } else { prev_scene };
            light_probes(scene, j, mix64(frame as u64 * 31 + j as u64), &mut ev.regions);
            ev.objs |= 1u128 << (LIGHT_ID_BASE + j);
            ev.lights |= 1 << j;
            ev.probe_rays += PROBES;
        }
    }
    Some(ev)
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
    pub delta_paths: usize,
    pub delta_old_paths: usize,
    pub delta_zero: usize,
    pub safety: usize,
    pub cells: usize,
    pub rmse: f64,
    pub relmse: f64,
    /// relMSE et biais relatif signé, pièce 0 (A) et autres pièces
    pub relmse_a: f64,
    pub relmse_b: f64,
    pub bias_a: f64,
    pub bias_b: f64,
}

impl FrameStats {
    pub const HEADER: &'static str = "frame,paths,paths_a,paths_b,probes,suspects,hard_resets,rejected,confirmed,\
delta_paths,delta_old_paths,delta_zero,safety,cells,rmse,relmse,relmse_a,relmse_b,bias_a,bias_b";
    pub fn csv(&self) -> String {
        format!(
            "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{:.6e},{:.6e},{:.6e},{:.6e},{:.6e},{:.6e}",
            self.frame,
            self.paths,
            self.paths_a,
            self.paths_b,
            self.probes,
            self.suspects,
            self.hard_resets,
            self.rejected,
            self.confirmed,
            self.delta_paths,
            self.delta_old_paths,
            self.delta_zero,
            self.safety,
            self.cells,
            self.rmse,
            self.relmse,
            self.relmse_a,
            self.relmse_b,
            self.bias_a,
            self.bias_b
        )
    }
}

pub const SAVE_FRAMES: [u32; 10] = [39, 41, 63, 79, 80, 81, 99, 100, 101, 119];

fn post_var(n: f64, rel: f64, prior: f64) -> f64 {
    if n < 1.0 { prior } else { (KAPPA * prior + (n - 1.0) * rel) / (KAPPA + n - 1.0) }
}

/// (n effectif, variance relative brute) d'une cellule, en tenant compte de l'état suspect.
fn uncertainty(c: &Cell) -> (f64, f64) {
    let (n, var) = match &c.fresh {
        Some(f) => (f.n, c.main.var().max(f.var())),
        None => (c.main.n, c.main.var()),
    };
    let m = c.main.lmean.max(c.fresh.map_or(0.0, |f| f.lmean));
    (n, var / (m * m + REL_EPS))
}

/// Gain marginal (réduction d'erreur image attendue) d'un échantillon de plus.
fn gain(foot: f64, pv: f64, n: f64) -> f64 {
    if n < 1.0 { foot * 1e6 } else { foot * pv / (n * (n + 1.0)) }
}

/// Mise à jour différentielle à partir de paires corrélées (x = nouvelle scène, y = ancienne scène,
/// même graine). L'historique sert de variable de contrôle :
///   μ = x̄ − β (ȳ − μ_cache),  Var μ ≈ Var(x − βy)/m + β² Var(μ_cache).
/// β = 1 : correction pure par Δ̂ ; β = 0 : redémarrage sur les échantillons frais.
/// Renvoie false si tous les Δ sont nuls (cellule inchangée : on garde tout).
fn apply_pairs(main: &mut Stat, pairs: &[(V3, V3)], cv: bool) -> bool {
    if pairs.iter().all(|(x, y)| x == y) {
        return false;
    }
    let m = pairs.len() as f64;
    let (mut xr, mut yr, mut xl, mut yl) = (V3::default(), V3::default(), 0.0, 0.0);
    for (x, y) in pairs {
        xr += *x / m;
        yr += *y / m;
        xl += x.lum() / m;
        yl += y.lum() / m;
    }
    let (mut sxx, mut syy, mut sxy) = (0.0, 0.0, 0.0);
    for (x, y) in pairs {
        let (dx, dy) = (x.lum() - xl, y.lum() - yl);
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
    }
    let mm = m * (m - 1.0);
    let var_hist = if main.n > 1.0 { main.var() / main.n } else { f64::INFINITY };
    let beta = if !cv {
        1.0
    } else if !var_hist.is_finite() {
        0.0
    } else {
        // β* minimise (Sxx − 2βSxy + β²Syy)/(m(m−1)) + β² Var(μ_cache)
        let denom = syy / mm + var_hist;
        if denom > 0.0 { (sxy / mm / denom).clamp(0.0, 1.0) } else { 1.0 }
    };
    let var_mu = (sxx - 2.0 * beta * sxy + beta * beta * syy).max(0.0) / mm
        + if beta > 0.0 { beta * beta * var_hist } else { 0.0 };
    let mu = (xr - (yr - main.mean) * beta).max(V3::default());
    let mu_l = (xl - beta * (yl - main.lmean)).max(0.0);
    let s2 = (sxx / (m - 1.0)).max(main.var() * 1e-3).max(1e-12);
    let n_eff = if var_mu > 0.0 { (s2 / var_mu).clamp(m.min(2.0), main.n + m) } else { main.n + m };
    *main = Stat { n: n_eff, mean: mu, lmean: mu_l, m2: s2 * (n_eff - 1.0) };
    true
}

fn task_rng(seed: u64, frame: u32, i: usize) -> Rng {
    Rng::new(mix64(seed ^ mix64((frame as u64) << 32 | i as u64)))
}

fn add_sample(c: &mut Cell, v: V3, rec: &DepRec, ema: bool, safety: bool) {
    c.deps.or(&rec.regions);
    c.objs |= rec.objs;
    if ema {
        c.main.add_ema(v, EMA_CACHE);
    } else if let Some(f) = c.fresh.as_mut() {
        f.add(v);
    } else {
        c.main.add(v);
        if safety {
            c.recent.add(v);
        }
    }
}

pub fn run(cfg: &Cfg, refs: &HashMap<String, Vec<V3>>, out_dir: &str, run_seed: u64, world: &World) -> Vec<FrameStats> {
    std::fs::create_dir_all(out_dir).unwrap();
    let seed = mix64(cfg.name.bytes().fold(7u64, |h, b| h.wrapping_mul(131) ^ b as u64) ^ mix64(run_seed));
    let mut stats = Vec::new();
    let mut prev_state = state_at(0);
    let mut prev_scene = build(&prev_state, world.rooms);
    let mut pixels: Vec<Stat> = vec![Stat::default(); world.len()];
    let mut cache = Cache::default();
    let mut rng = Rng::new(seed);
    let mut debt = 0usize; // chemins différentiels à rembourser sur le budget des frames suivantes

    for frame in 0..FRAMES {
        let state = state_at(frame);
        let scene = build(&state, world.rooms);
        let mut fs = FrameStats { frame, ..Default::default() };
        let ev = if frame > 0 { event(&prev_state, &state, &prev_scene, &scene, frame) } else { None };
        let gb = gbuffer(&scene, world);
        let surf: Vec<usize> = (0..world.len()).filter(|&k| matches!(gb[k], Px::Surf { .. })).collect();
        let budget = surf.len();
        let mut img = vec![V3::default(); world.len()];
        for k in 0..world.len() {
            if let Px::Light(e) = gb[k] {
                img[k] = e;
            }
        }

        match cfg.kind {
            Kind::PixelReset | Kind::PixelEma => {
                if matches!(cfg.kind, Kind::PixelReset) && ev.is_some() {
                    pixels.iter_mut().for_each(|s| *s = Stat::default());
                }
                let samples = par_map(surf.len(), |i| {
                    let Px::Surf { p, n, albedo, .. } = gb[surf[i]] else { unreachable!() };
                    let mut r = task_rng(seed, frame, i);
                    radiance(&scene, p, n, albedo, &mut r, &mut NoRec, false)
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
                // 1. cellules visibles
                let mut pix_cells = vec![[u32::MAX; K]; world.len()];
                for &k in &surf {
                    let Px::Surf { p, n, dist, .. } = gb[k] else { unreachable!() };
                    for s in 0..K {
                        pix_cells[k][s] = cache.get_or_create(cell_key(p, n, dist * world.px_angle(), k, s), p.x);
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
                let mut tasks: Vec<(u32, u32)> = Vec::new();
                let pick = |c: usize, rng: &mut Rng| -> (u32, u32) {
                    let r = &refs[c];
                    (c as u32, r[rng.below(r.len())])
                };

                // 2. événement
                let mut delta_tasks: Vec<(u32, u32)> = Vec::new();
                if let Some(e) = &ev {
                    fs.probes = e.probe_rays;
                    for (ci, c) in cache.cells.iter_mut().enumerate() {
                        let deps = match on_event {
                            OnEvent::Nothing => continue,
                            OnEvent::GlobalReset => {
                                c.clear();
                                continue;
                            }
                            OnEvent::Invalidate(deps, _) => deps,
                        };
                        let suspect = match deps {
                            Deps::Spatial => c.deps.intersects(&e.regions),
                            Deps::Objects => c.objs & e.objs != 0,
                            Deps::All => true,
                        };
                        if !suspect || c.main.n == 0.0 {
                            continue;
                        }
                        fs.suspects += 1;
                        let OnEvent::Invalidate(_, action) = on_event else { unreachable!() };
                        match action {
                            Action::Hard => {
                                c.clear();
                                fs.hard_resets += 1;
                            }
                            Action::ZTest => {
                                c.main.cap(N_CAP);
                                c.fresh = Some(Stat::default());
                                c.recent = Stat::default();
                            }
                            Action::Delta { .. } => {
                                if foot[ci] > 0.0 {
                                    for _ in 0..M_DELTA0 {
                                        delta_tasks.push(pick(ci, &mut rng));
                                    }
                                } else {
                                    c.clear(); // invisible : on ne peut pas le mesurer, ça ne coûte rien
                                    fs.hard_resets += 1;
                                }
                            }
                        }
                    }
                }
                // 2b. chemins corrélés : même graine dans l'ancienne et la nouvelle scène.
                // Si le nouveau chemin ne touche ni la zone changée ni une lampe changée, l'ancien
                // chemin est identique : Δ = 0 exactement, sans second tracé.
                // Deux tours : M_DELTA0 chemins, puis M_DELTA au total pour les cellules dont un chemin est affecté.
                if !delta_tasks.is_empty() {
                    let e = ev.as_ref().unwrap();
                    let cv = matches!(on_event, OnEvent::Invalidate(_, Action::Delta { cv: true }));
                    let trace = |tasks: &[(u32, u32)], round: u64| {
                        par_map(tasks.len(), |i| {
                            let Px::Surf { p, n, albedo, .. } = gb[tasks[i].1 as usize] else { unreachable!() };
                            let s = mix64(seed ^ 0xde17a ^ mix64((frame as u64) << 32 | round << 24 | i as u64));
                            let mut rec = DepRec::default();
                            if !e.geometry {
                                // lampes seules : l'ancienne radiance se déduit du même chemin
                                let (new, d) =
                                    radiance_split(&scene, p, n, albedo, &mut Rng::new(s), &mut rec, true, &e.light_w);
                                let affected = rec.lights & e.lights != 0;
                                return (tasks[i].0, new, new + d, affected, rec, false);
                            }
                            let new = radiance(&scene, p, n, albedo, &mut Rng::new(s), &mut rec, true);
                            let affected = rec.regions.intersects(&e.regions) || rec.lights & e.lights != 0;
                            let old = if affected {
                                radiance(&prev_scene, p, n, albedo, &mut Rng::new(s), &mut NoRec, true)
                            } else {
                                new
                            };
                            (tasks[i].0, new, old, affected, rec, affected)
                        })
                    };
                    let mut results = trace(&delta_tasks, 0);
                    let mut second: Vec<(u32, u32)> = Vec::new();
                    let mut seen = std::collections::HashSet::new();
                    // Δ = 0 sur M_DELTA0 chemins ne prouve rien pour une lampe (sa sélection NEE est aléatoire,
                    // indépendante de son importance pour la cellule) : changement de lampes => M_DELTA partout.
                    for r in &results {
                        if (r.3 || !e.geometry) && seen.insert(r.0) {
                            for _ in 0..M_DELTA - M_DELTA0 {
                                second.push(pick(r.0 as usize, &mut rng));
                            }
                        }
                    }
                    results.extend(trace(&second, 1));
                    let mut per_cell: HashMap<u32, Vec<usize>> = HashMap::new();
                    for (i, r) in results.iter().enumerate() {
                        per_cell.entry(r.0).or_default().push(i);
                        fs.delta_paths += 1 + r.5 as usize;
                        fs.delta_old_paths += r.5 as usize;
                    }
                    for (ci, idx) in per_cell {
                        let c = &mut cache.cells[ci as usize];
                        let pairs: Vec<(V3, V3)> = idx.iter().map(|&i| (results[i].1, results[i].2)).collect();
                        let changed = apply_pairs(&mut c.main, &pairs, cv);
                        c.recent = Stat::default();
                        for &i in &idx {
                            let r = &results[i];
                            if changed {
                                c.deps.or(&r.4.regions);
                                c.objs |= r.4.objs;
                            } else {
                                add_sample(c, r.1, &r.4, ema, safety);
                            }
                        }
                        if !changed {
                            fs.delta_zero += 1;
                        }
                        c.last = frame as i64;
                    }
                }
                if cfg.target.is_none() {
                    debt += fs.delta_paths;
                }

                // 3. allocation
                if !neyman {
                    for &k in &surf {
                        tasks.push((pix_cells[k][rng.below(K)], k as u32));
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
                        None => {
                            let pay = debt.min((budget as f64 * DEBT_RATE) as usize);
                            debt -= pay;
                            budget - pay
                        }
                    };
                    // plancher d'exploration
                    for &c in &visible {
                        if frame as i64 - cache.cells[c].last >= EXPLORE_PERIOD && cache.cells[c].main.n > 0.0 {
                            tasks.push(pick(c, &mut rng));
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
                                tasks.push(pick(c, &mut rng));
                            }
                        }
                    } else {
                        let mut left = remaining;
                        while left > 0 {
                            let Some((_, c)) = heap.pop() else { break };
                            tasks.push(pick(c, &mut rng));
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
                    let v = radiance(&scene, p, n, albedo, &mut r, &mut rec, false);
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
                    if c.cx < 5.2 {
                        fs.paths_a += 1;
                    } else {
                        fs.paths_b += 1;
                    }
                    add_sample(c, *v, rec, ema, safety);
                }
                fs.paths = tasks.len() + fs.delta_paths + fs.probes;
                // 6. vérification (z-test) et filet de sécurité
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
        let r = &refs[&state.id()];
        let (mut acc, mut cnt) = ([0.0f64; 4], [0.0f64; 2]);
        for &k in &surf {
            let side = if gb[k].is_surf_a() { 0 } else { 1 };
            for c in 0..3 {
                let (x, y) = (img[k].get(c), r[k].get(c));
                acc[side] += (x - y) * (x - y) / (y * y + 0.01);
                acc[2 + side] += (x - y) / (y + 0.1);
            }
            cnt[side] += 3.0;
        }
        fs.relmse_a = acc[0] / cnt[0].max(1.0);
        fs.relmse_b = acc[1] / cnt[1].max(1.0);
        fs.bias_a = acc[2] / cnt[0].max(1.0);
        fs.bias_b = acc[3] / cnt[1].max(1.0);
        if run_seed == 0 && SAVE_FRAMES.contains(&frame) {
            crate::image::save(&format!("{out_dir}/f{frame:03}.png"), world.w, world.h, &img);
            // carte d'erreur relative (racine du relMSE par pixel, 1.0 = 100 %)
            let r = &refs[&state.id()];
            let err: Vec<V3> = (0..world.len())
                .map(|k| {
                    let e = (0..3).map(|c| (img[k].get(c) - r[k].get(c)).powi(2) / (r[k].get(c).powi(2) + 0.01)).sum::<f64>() / 3.0;
                    let e = e.sqrt() * 2.0;
                    v3(e, e * 0.35, 0.0)
                })
                .collect();
            crate::image::save(&format!("{out_dir}/e{frame:03}.png"), world.w, world.h, &err);
        }
        stats.push(fs);
        prev_state = state;
        prev_scene = scene;
    }
    stats
}

impl Px {
    fn is_surf_a(&self) -> bool {
        matches!(self, Px::Surf { p, .. } if p.x < 5.2)
    }
}

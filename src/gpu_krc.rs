//! Pilote du pipeline KRC entièrement GPU (krc_gpu.wgsl). Par frame, le CPU n'envoie que la
//! scène (quelques centaines d'octets) et, s'il y a lieu, la description de l'événement ; il ne
//! relit que l'image et quelques compteurs, pour l'évaluation (hors chronométrage GPU).
use crate::deps::{Coarse, SUPER, WORDS};
use crate::gpu::{f3, scene_to_gpu, GBox, GLight, Gpu, MAX_BOXES, MAX_LIGHTS};
use crate::math::{v3, V3};
use crate::methods::Event;
use crate::render::World;
use crate::scene::Scene;

const K: usize = 4;
const ACC: usize = 44;
const CELL_BYTES: usize = 144;
const CTR: usize = 256;

// indices des compteurs (doivent suivre krc_gpu.wgsl)
const C_CELLS: usize = 0;
const C_TASKS: usize = 1;
const C_SURF: usize = 2;
const C_SUSPECTS: usize = 3;
const C_DPATHS: usize = 4;
const C_DOLD: usize = 5;
const C_DZERO: usize = 6;
const C_HARD: usize = 7;
const C_SAFETY: usize = 8;
const C_REG0: usize = 12;
const C_VISIBLE: usize = 13;
const C_OVERFLOW: usize = 15;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct FrameU {
    w: u32,
    h: u32,
    n_boxes: u32,
    n_lights: u32,
    frame: u32,
    seed: u32,
    flags: u32,
    table_mask: u32,
    cell_cap: u32,
    round: u32,
    ev_lights: u32,
    pad0: u32,
    eye: [f32; 3],
    px_angle: f32,
    fwd: [f32; 3],
    th: f32,
    right: [f32; 3],
    tv: f32,
    up: [f32; 3],
    pad1: f32,
}

const F_EVENT: u32 = 1;
const F_GEOMETRY: u32 = 2;
const F_SHARC: u32 = 4;

/// Résultat d'une frame.
pub struct GpuFrame {
    pub image: Vec<V3>,
    pub cells: u32,
    pub visible: u32,
    pub surf: u32,
    pub regular_paths: u32,
    pub suspects: u32,
    pub delta_paths: u32,
    pub delta_old: u32,
    pub delta_zero: u32,
    pub hard: u32,
    pub safety: u32,
    pub overflow: u32,
    pub max_n: f32,
    pub debt: u32,
    /// durée GPU de chaque passe (timestamps), en ms
    pub passes: Vec<(&'static str, f64)>,
}

impl GpuFrame {
    pub fn gpu_ms(&self) -> f64 {
        self.passes.iter().map(|p| p.1).sum()
    }
}

pub struct GpuKrc<'a> {
    gpu: &'a Gpu,
    world: World,
    sharc: bool,
    pipes: std::collections::HashMap<&'static str, wgpu::ComputePipeline>,
    bufs: Vec<wgpu::Buffer>,
    bind: wgpu::BindGroup,
    read_img: wgpu::Buffer,
    read_ctr: wgpu::Buffer,
    ts: wgpu::QuerySet,
    ts_resolve: wgpu::Buffer,
    ts_read: wgpu::Buffer,
    period_ns: f32,
    cell_cap: u32,
    table_mask: u32,
    seed: u32,
    /// relire l'image à chaque frame (évaluation) ; le banc de vitesse s'en passe
    pub readback: bool,
}

const ENTRIES: [&str; 26] = [
    "reset_ctr",
    "prep_cells",
    "gbuffer",
    "insert",
    "lookup",
    "event_mark",
    "prep_d1",
    "delta_trace1",
    "delta_more",
    "prep_d2",
    "delta_trace2",
    "delta_apply",
    "alloc_hist",
    "alloc_prep",
    "alloc_scan1",
    "alloc_pick1",
    "alloc_scan2",
    "alloc_pick2",
    "alloc_scan3",
    "alloc_pick3",
    "alloc_emit",
    "sharc_emit",
    "prep_reg",
    "trace",
    "resolve",
    "display",
];
const MAX_PASSES: u32 = 32;

impl<'a> GpuKrc<'a> {
    pub fn new(gpu: &'a Gpu, world: World, sharc: bool, seed: u32) -> GpuKrc<'a> {
        let dev = &gpu.device;
        let len = world.len();
        // une cellule couvre 2 à 4 pixels de large ; avec le jitter, ≈ len/6 cellules distinctes
        let cell_cap = (len / 2).next_power_of_two().max(1 << 14) as u32;
        let table = (cell_cap as usize * 2).next_power_of_two();
        let task_cap = cell_cap as usize * 10 + len * 2;
        let storage = |read_only: bool| wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Storage { read_only },
            has_dynamic_offset: false,
            min_binding_size: None,
        };
        let mut entries = Vec::new();
        for b in 0..18u32 {
            let ty = match b {
                0 => wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                1..=4 | 14 => storage(true),
                _ => storage(false),
            };
            entries.push(wgpu::BindGroupLayoutEntry { binding: b, visibility: wgpu::ShaderStages::COMPUTE, ty, count: None });
        }
        let layout = dev.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some("krc"), entries: &entries });
        let pl = dev.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("krc"),
            bind_group_layouts: &[&layout],
            push_constant_ranges: &[],
        });
        let module = dev.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("krc_gpu"),
            source: wgpu::ShaderSource::Wgsl(include_str!("krc_gpu.wgsl").into()),
        });
        let mut pipes = std::collections::HashMap::new();
        for e in ENTRIES {
            pipes.entry(e).or_insert_with(|| {
                dev.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                    label: Some(e),
                    layout: Some(&pl),
                    module: &module,
                    entry_point: Some(e),
                    compilation_options: Default::default(),
                    cache: None,
                })
            });
        }
        let st = wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC;
        let sizes: [(usize, wgpu::BufferUsages); 18] = [
            (std::mem::size_of::<FrameU>(), wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST),
            (MAX_BOXES * std::mem::size_of::<GBox>(), st),
            (MAX_LIGHTS * std::mem::size_of::<GLight>(), st),
            (MAX_BOXES * std::mem::size_of::<GBox>(), st),
            (MAX_LIGHTS * std::mem::size_of::<GLight>(), st),
            (len * 48, st),
            (len * K * 4, st),
            (table * 4, st),
            (table * 4, st),
            (CTR * 4, st),
            (cell_cap as usize * CELL_BYTES, st),
            (cell_cap as usize * ACC * 4, st),
            (task_cap * 16, st),
            (len * 16, st),
            (162 * 4, st),
            (32, st),
            (48, st | wgpu::BufferUsages::INDIRECT),
            (cell_cap as usize * 16, st),
        ];
        let bufs: Vec<wgpu::Buffer> = sizes
            .iter()
            .enumerate()
            .map(|(i, &(size, usage))| {
                dev.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(&format!("b{i}")),
                    size: size as u64,
                    usage,
                    mapped_at_creation: false,
                })
            })
            .collect();
        let bind = dev.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("krc"),
            layout: &layout,
            entries: &bufs
                .iter()
                .enumerate()
                .map(|(i, b)| wgpu::BindGroupEntry { binding: i as u32, resource: b.as_entire_binding() })
                .collect::<Vec<_>>(),
        });
        let mk = |size: u64, usage: wgpu::BufferUsages| {
            dev.create_buffer(&wgpu::BufferDescriptor { label: None, size, usage, mapped_at_creation: false })
        };
        let read = wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST;
        GpuKrc {
            gpu,
            world,
            sharc,
            pipes,
            read_img: mk((len * 16) as u64, read),
            read_ctr: mk((CTR * 4) as u64, read),
            ts: dev.create_query_set(&wgpu::QuerySetDescriptor { label: None, ty: wgpu::QueryType::Timestamp, count: MAX_PASSES * 2 }),
            ts_resolve: mk(
                MAX_PASSES as u64 * 16,
                wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
            ),
            ts_read: mk(MAX_PASSES as u64 * 16, read),
            period_ns: gpu.queue.get_timestamp_period(),
            bufs,
            bind,
            cell_cap,
            table_mask: table as u32 - 1,
            seed,
            readback: true,
        }
    }

    /// Une frame complète. `t` : indice de frame (graines, exploration).
    pub fn frame(&mut self, scene: &Scene, prev: &Scene, ev: Option<&Event>, t: u32) -> GpuFrame {
        let q = &self.gpu.queue;
        let w = &self.world;
        let len = w.len() as u32;
        let (eye, fwd, right, up, th, tv) = w.camera();
        let mut flags = if self.sharc { F_SHARC } else { 0 };
        let mut ev_lights = 0;
        if let Some(e) = ev {
            if !self.sharc {
                flags |= F_EVENT;
                if e.geometry {
                    flags |= F_GEOMETRY;
                }
                ev_lights = e.lights as u32;
                q.write_buffer(&self.bufs[14], 0, bytemuck::cast_slice(&event_words(e)));
            }
        }
        let fu = FrameU {
            w: w.w as u32,
            h: w.h as u32,
            n_boxes: scene.objects.len() as u32,
            n_lights: scene.lights.len() as u32,
            frame: t,
            seed: self.seed,
            flags,
            table_mask: self.table_mask,
            cell_cap: self.cell_cap,
            round: 0,
            ev_lights,
            pad0: 0,
            eye: f3(eye),
            px_angle: w.px_angle() as f32,
            fwd: f3(fwd),
            th: th as f32,
            right: f3(right),
            tv: tv as f32,
            up: f3(up),
            pad1: 0.0,
        };
        q.write_buffer(&self.bufs[0], 0, bytemuck::bytes_of(&fu));
        let w_ev: Vec<f64> = ev.map(|e| e.light_w.clone()).unwrap_or_default();
        let (b, l) = scene_to_gpu(scene, &w_ev);
        let (bp, lp) = scene_to_gpu(prev, &w_ev);
        q.write_buffer(&self.bufs[1], 0, bytemuck::cast_slice(&b));
        q.write_buffer(&self.bufs[2], 0, bytemuck::cast_slice(&l));
        q.write_buffer(&self.bufs[3], 0, bytemuck::cast_slice(&bp));
        q.write_buffer(&self.bufs[4], 0, bytemuck::cast_slice(&lp));

        let mut plan: Vec<(&'static str, Dispatch)> = Vec::new();
        let px = Dispatch::Direct(len.div_ceil(64));
        let slots = Dispatch::Direct((len * K as u32).div_ceil(256));
        let cells = Dispatch::Indirect(36);
        plan.push(("reset_ctr", Dispatch::Direct((CTR as u32).div_ceil(64))));
        plan.push(("gbuffer", px));
        plan.push(("insert", slots));
        plan.push(("lookup", slots));
        plan.push(("prep_cells", Dispatch::Direct(1)));
        if flags & F_EVENT != 0 {
            plan.push(("event_mark", cells));
            plan.push(("prep_d1", Dispatch::Direct(1)));
            plan.push(("delta_trace1", Dispatch::Indirect(0)));
            if flags & F_GEOMETRY != 0 {
                plan.push(("delta_more", cells));
                plan.push(("prep_d2", Dispatch::Direct(1)));
                plan.push(("delta_trace2", Dispatch::Indirect(12)));
            }
            plan.push(("delta_apply", cells));
        }
        if self.sharc {
            plan.push(("sharc_emit", px));
        } else {
            plan.push(("alloc_hist", cells));
            plan.push(("alloc_prep", Dispatch::Direct(1)));
            plan.push(("alloc_scan1", cells));
            plan.push(("alloc_pick1", Dispatch::Direct(1)));
            plan.push(("alloc_scan2", cells));
            plan.push(("alloc_pick2", Dispatch::Direct(1)));
            plan.push(("alloc_scan3", cells));
            plan.push(("alloc_pick3", Dispatch::Direct(1)));
            plan.push(("alloc_emit", cells));
        }
        plan.push(("prep_reg", Dispatch::Direct(1)));
        plan.push(("trace", Dispatch::Indirect(24)));
        plan.push(("resolve", cells));
        plan.push(("display", px));

        let dev = &self.gpu.device;
        let mut enc = dev.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("krc frame") });
        for (i, (name, d)) in plan.iter().enumerate() {
            let tw = wgpu::ComputePassTimestampWrites {
                query_set: &self.ts,
                beginning_of_pass_write_index: Some(2 * i as u32),
                end_of_pass_write_index: Some(2 * i as u32 + 1),
            };
            let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some(name), timestamp_writes: Some(tw) });
            pass.set_pipeline(&self.pipes[name]);
            pass.set_bind_group(0, &self.bind, &[]);
            match *d {
                Dispatch::Direct(n) => pass.dispatch_workgroups(n, 1, 1),
                Dispatch::Indirect(off) => pass.dispatch_workgroups_indirect(&self.bufs[16], off),
            }
        }
        let np = plan.len() as u32;
        enc.resolve_query_set(&self.ts, 0..2 * np, &self.ts_resolve, 0);
        enc.copy_buffer_to_buffer(&self.ts_resolve, 0, &self.ts_read, 0, np as u64 * 16);
        if self.readback {
            enc.copy_buffer_to_buffer(&self.bufs[13], 0, &self.read_img, 0, len as u64 * 16);
        }
        enc.copy_buffer_to_buffer(&self.bufs[9], 0, &self.read_ctr, 0, (CTR * 4) as u64);
        q.submit(Some(enc.finish()));
        let reads: Vec<&wgpu::Buffer> =
            if self.readback { vec![&self.read_img, &self.read_ctr, &self.ts_read] } else { vec![&self.read_ctr, &self.ts_read] };
        for b in &reads {
            b.slice(..).map_async(wgpu::MapMode::Read, |r| r.expect("lecture GPU"));
        }
        dev.poll(wgpu::Maintain::Wait);
        let img: Vec<f32> = if self.readback {
            bytemuck::cast_slice(&self.read_img.slice(..).get_mapped_range()).to_vec()
        } else {
            Vec::new()
        };
        let ctr: Vec<u32> = bytemuck::cast_slice(&self.read_ctr.slice(..).get_mapped_range()).to_vec();
        let ts: Vec<u64> = bytemuck::cast_slice(&self.ts_read.slice(..).get_mapped_range()).to_vec();
        for b in &reads {
            b.unmap();
        }
        let ms = self.period_ns as f64 * 1e-6;
        GpuFrame {
            image: (0..img.len() / 4).map(|k| v3(img[4 * k] as f64, img[4 * k + 1] as f64, img[4 * k + 2] as f64)).collect(),
            cells: ctr[C_CELLS],
            visible: ctr[C_VISIBLE],
            surf: ctr[C_SURF],
            regular_paths: ctr[C_TASKS] - ctr[C_REG0],
            suspects: ctr[C_SUSPECTS],
            delta_paths: ctr[C_DPATHS],
            delta_old: ctr[C_DOLD],
            delta_zero: ctr[C_DZERO],
            hard: ctr[C_HARD],
            safety: ctr[C_SAFETY],
            overflow: ctr[C_OVERFLOW],
            max_n: f32::from_bits(ctr[17]),
            debt: ctr[9],
            passes: plan
                .iter()
                .enumerate()
                .map(|(i, (name, _))| (*name, ts[2 * i + 1].saturating_sub(ts[2 * i]) as f64 * ms))
                .collect(),
        }
    }
}

/// `krc gpubench` : temps GPU par frame du pipeline complet, en montant en résolution.
pub fn bench(gpu: &Gpu) {
    use crate::methods::event;
    use crate::scene::{build, state_at, FRAMES};
    let mut csv = String::from("scale,width,height,method,phase,mean_ms,max_ms,paths_per_frame
");
    for s in [1usize, 2, 5, 10] {
        let world = World::scaled(2, s);
        for sharc in [false, true] {
            let name = if sharc { "gpu_sharc" } else { "gpu_krc" };
            let mut krc = GpuKrc::new(gpu, world, sharc, 1);
            krc.readback = false;
            let mut prev_state = state_at(0);
            let mut prev_scene = build(&prev_state, 2);
            let mut per_frame: Vec<(u32, f64, u32)> = Vec::new();
            let mut passes: Vec<(&'static str, f64, usize)> = Vec::new();
            // une séquence de chauffe (fréquences GPU, caches), puis la séquence mesurée
            for t in 0..2 * FRAMES {
                let warm = t < FRAMES;
                let t = t % FRAMES;
                let state = state_at(t);
                let scene = build(&state, 2);
                let ev = event(&prev_state, &state, &prev_scene, &scene, t);
                let f = krc.frame(&scene, &prev_scene, ev.as_ref(), t);
                assert_eq!(f.overflow, 0, "table saturée ({} cellules)", f.cells);
                prev_state = state;
                prev_scene = scene;
                if warm {
                    continue;
                }
                per_frame.push((t, f.gpu_ms(), f.regular_paths + f.delta_paths));
                for (n, ms) in &f.passes {
                    match passes.iter_mut().find(|p| p.0 == *n) {
                        Some(p) => {
                            p.1 += ms;
                            p.2 += 1;
                        }
                        None => passes.push((n, *ms, 1)),
                    }
                }
            }
            let mut line = format!("{}×{} {name:<9}", world.w, world.h);
            for (phase, a, b) in [("statique", 20u32, 40u32), ("boite", 40, 64), ("lampe_on", 80, 81), ("lampe_off", 100, 101), ("tout", 0, 120)] {
                let sel: Vec<&(u32, f64, u32)> = per_frame.iter().filter(|p| p.0 >= a && p.0 < b).collect();
                // médiane : robuste aux fluctuations de fréquence du GPU
                let mut v: Vec<f64> = sel.iter().map(|p| p.1).collect();
                v.sort_by(|x, y| x.total_cmp(y));
                let mean = v[v.len() / 2];
                let max = sel.iter().map(|p| p.1).fold(0.0, f64::max);
                let paths = sel.iter().map(|p| p.2 as f64).sum::<f64>() / sel.len() as f64;
                csv += &format!("{s},{},{},{name},{phase},{mean:.4},{max:.4},{paths:.0}
", world.w, world.h);
                line += &format!(" | {phase} {mean:6.3} ms");
            }
            println!("{line}");
            if s == 10 {
                let mut p: Vec<String> = passes.iter().map(|(n, ms, c)| format!("{n} {:.3}", ms / *c as f64)).collect();
                p.sort();
                println!("   passes (ms, moyenne sur les frames où elles tournent) : {}", p.join(", "));
            }
        }
    }
    std::fs::create_dir_all("results/gpu").unwrap();
    std::fs::write("results/gpu/bench_pipeline.csv", csv).unwrap();
}

/// `krc drift [cycles]` : la séquence de 120 frames répétée ; erreur mesurée aux mêmes instants
/// de chaque cycle pour détecter une dérive (stagnation f32, saturation, dette, table).
pub fn drift(gpu: &Gpu, cycles: u32, refs: &std::collections::HashMap<String, Vec<V3>>) {
    use crate::methods::event;
    use crate::render::{errors, gbuffer, Px};
    use crate::scene::{build, state_at, FRAMES};
    let world = World::new(2);
    let probe_frames = [39u32, 63, 79, 81, 99, 101, 119];
    let mut csv = String::from("method,cycle,frame,relmse,cells,max_n,debt,overflow
");
    for sharc in [false, true] {
        let name = if sharc { "gpu_sharc" } else { "gpu_krc" };
        let mut krc = GpuKrc::new(gpu, world, sharc, 7);
        let mut prev_state = state_at(0);
        let mut prev_scene = build(&prev_state, 2);
        let mut first: Vec<f64> = Vec::new();
        let mut last: Vec<f64> = Vec::new();
        for cycle in 0..cycles {
            let mut row = Vec::new();
            for t in 0..FRAMES {
                let state = state_at(t);
                let scene = build(&state, 2);
                let ev = event(&prev_state, &state, &prev_scene, &scene, t);
                krc.readback = probe_frames.contains(&t);
                let f = krc.frame(&scene, &prev_scene, ev.as_ref(), cycle * FRAMES + t);
                if krc.readback {
                    let gb = gbuffer(&scene, &world);
                    let mut img = f.image;
                    for k in 0..world.len() {
                        if let Px::Light(e) = gb[k] {
                            img[k] = e;
                        }
                    }
                    let (_, rel) = errors(&img, &refs[&state.id()], &gb);
                    csv += &format!("{name},{cycle},{t},{rel:.6e},{},{},{},{}
", f.cells, f.max_n, f.debt, f.overflow);
                    row.push(rel);
                    if t == 119 && (cycle % 10 == 9 || cycle == 0) {
                        println!(
                            "{name} cycle {cycle:>3} : relMSE aux frames {probe_frames:?} = {} | cellules {} | n max {:.0} | dette {} | débordements {}",
                            row.iter().map(|r| format!("{r:.2e}")).collect::<Vec<_>>().join(" "),
                            f.cells,
                            f.max_n,
                            f.debt,
                            f.overflow
                        );
                    }
                }
                prev_state = state;
                prev_scene = scene;
            }
            if cycle < 10 {
                first.push(row.iter().sum::<f64>() / row.len() as f64);
            }
            if cycle >= cycles.saturating_sub(10) {
                last.push(row.iter().sum::<f64>() / row.len() as f64);
            }
        }
        let m = |v: &Vec<f64>| v.iter().sum::<f64>() / v.len() as f64;
        println!("{name} : relMSE moyen aux instants sondés, 10 premiers cycles {:.3e} -> 10 derniers {:.3e}", m(&first), m(&last));
    }
    std::fs::create_dir_all("results/gpu").unwrap();
    std::fs::write("results/gpu/drift.csv", csv).unwrap();
}

#[derive(Clone, Copy)]
enum Dispatch {
    Direct(u32),
    /// décalage (octets) des arguments dans le buffer indirect
    Indirect(u64),
}

/// Description d'un événement pour le GPU : régions fines, super-régions, magnitudes, poids des lampes.
fn event_words(e: &Event) -> Vec<u32> {
    let mut v = vec![0u32; 162];
    for k in 0..WORDS {
        v[2 * k] = e.regions.0[k] as u32;
        v[2 * k + 1] = (e.regions.0[k] >> 32) as u32;
    }
    let l2 = Coarse::from(&e.regions).l2;
    v[108] = l2 as u32;
    v[109] = (l2 >> 32) as u32;
    let mut mag = [0.0f32; SUPER];
    for &(r, m) in &e.mag {
        let s = crate::deps::super_of(r);
        mag[s] = mag[s].max(m as f32);
    }
    for s in 0..SUPER {
        v[110 + s] = mag[s].to_bits();
    }
    for (j, &w) in e.light_w.iter().enumerate().take(16) {
        v[146 + j] = (w as f32).to_bits();
    }
    v
}

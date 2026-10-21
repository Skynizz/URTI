//! Traceur GPU : compute shader WGSL (trace.wgsl) via wgpu (Vulkan/DX12).
use crate::deps::{Bits, WORDS};
use crate::math::{mix64, v3, V3};
use crate::pt::DepRec;
use crate::scene::Scene;
use crate::tracer::{Opts, Req, Traced};

const WG: u32 = 64;
const MAX_BATCH: usize = 131_072;
/// chemins par requête et par dispatch (au-delà : plusieurs dispatchs, évite le watchdog du pilote)
const SPP_CHUNK: u32 = 64;
const MASKS: usize = 8;
const REGION_WORDS: usize = WORDS * 2; // u64 -> u32

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub(crate) struct GBox {
    mn: [f32; 3],
    p0: f32,
    mx: [f32; 3],
    p1: f32,
    albedo: [f32; 3],
    p2: f32,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub(crate) struct GLight {
    center: [f32; 3],
    hx: f32,
    hz: f32,
    on: f32,
    power: f32,
    toggle_w: f32,
    emission: [f32; 3],
    p0: f32,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct GTask {
    p: [f32; 3],
    seed_lo: u32,
    n: [f32; 3],
    seed_hi: u32,
    albedo: [f32; 3],
    p0: u32,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Params {
    n_tasks: u32,
    n_boxes: u32,
    n_lights: u32,
    flags: u32,
    spp: u32,
    pad: [u32; 3],
}

/// Scène au format GPU ; `toggle_w` : poids allumée_avant − allumée_après par lampe.
pub(crate) fn scene_to_gpu(scene: &Scene, toggle_w: &[f64]) -> (Vec<GBox>, Vec<GLight>) {
    let boxes = scene
        .objects
        .iter()
        .map(|b| GBox { mn: f3(b.aabb.min), p0: 0.0, mx: f3(b.aabb.max), p1: 0.0, albedo: f3(b.albedo), p2: 0.0 })
        .collect();
    let lights = scene
        .lights
        .iter()
        .enumerate()
        .map(|(j, l)| GLight {
            center: f3(l.center),
            hx: l.hx as f32,
            hz: l.hz as f32,
            on: l.on as u8 as f32,
            power: scene.light_power_if_on(j) as f32,
            toggle_w: toggle_w.get(j).copied().unwrap_or(0.0) as f32,
            emission: f3(l.emission),
            p0: 0.0,
        })
        .collect();
    (boxes, lights)
}

pub(crate) fn f3(v: V3) -> [f32; 3] {
    [v.x as f32, v.y as f32, v.z as f32]
}

/// Buffers persistants (agrandis si nécessaire) : les recréer à chaque appel coûtait ~85 ms.
struct Bufs {
    cap: usize,
    params: wgpu::Buffer,
    boxes: wgpu::Buffer,
    lights: wgpu::Buffer,
    tasks: wgpu::Buffer,
    outs: Vec<wgpu::Buffer>,
    read: Vec<wgpu::Buffer>,
    bind: wgpu::BindGroup,
}

pub(crate) const MAX_BOXES: usize = 64;
pub(crate) const MAX_LIGHTS: usize = 16;

/// Profil cumulé des appels (ms) : où passe le temps d'un lot.
#[derive(Default, Clone, Debug)]
pub struct Prof {
    pub calls: usize,
    /// préparation des tâches et de la scène côté CPU
    pub build: f64,
    /// write_buffer (copie vers la zone de transfert)
    pub upload: f64,
    /// submit → fin du travail GPU (noyau + copies + synchronisation)
    pub wait: f64,
    /// noyau seul (timestamps GPU)
    pub kernel: f64,
    /// lecture des buffers mappés
    pub read: f64,
    /// conversion en structures CPU (V3 f64, DepRec)
    pub convert: f64,
}

impl Prof {
    pub fn total(&self) -> f64 {
        self.build + self.upload + self.wait + self.read + self.convert
    }
}

struct Timestamps {
    set: wgpu::QuerySet,
    resolve: wgpu::Buffer,
    read: wgpu::Buffer,
    period_ns: f32,
}

pub struct Gpu {
    pub(crate) device: wgpu::Device,
    pub(crate) queue: wgpu::Queue,
    pipeline: wgpu::ComputePipeline,
    bufs: std::sync::Mutex<Option<Bufs>>,
    ts: Option<Timestamps>,
    pub prof: std::sync::Mutex<Prof>,
    pub name: String,
}

impl Gpu {
    pub fn new() -> Option<Gpu> {
        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN | wgpu::Backends::DX12,
            ..Default::default()
        });
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: None,
            force_fallback_adapter: false,
        }))?;
        let info = adapter.get_info();
        if info.device_type == wgpu::DeviceType::Cpu {
            return None;
        }
        // timestamps, et ray tracing matériel si l'adaptateur le propose (Vulkan sur RTX)
        let features = adapter.features()
            & (wgpu::Features::TIMESTAMP_QUERY
                | wgpu::Features::EXPERIMENTAL_RAY_TRACING_ACCELERATION_STRUCTURE
                | wgpu::Features::EXPERIMENTAL_RAY_QUERY);
        let (device, queue) = pollster::block_on(adapter.request_device(
            &wgpu::DeviceDescriptor {
                label: Some("krc"),
                required_features: features,
                required_limits: adapter.limits(),
                memory_hints: wgpu::MemoryHints::Performance,
            },
            None,
        ))
        .ok()?;
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("trace"),
            source: wgpu::ShaderSource::Wgsl(include_str!("trace.wgsl").into()),
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("trace"),
            layout: None,
            module: &module,
            entry_point: Some("main"),
            compilation_options: Default::default(),
            cache: None,
        });
        let ts = features.contains(wgpu::Features::TIMESTAMP_QUERY).then(|| Timestamps {
            set: device.create_query_set(&wgpu::QuerySetDescriptor { label: Some("ts"), ty: wgpu::QueryType::Timestamp, count: 2 }),
            resolve: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("ts"),
                size: 16,
                usage: wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: false,
            }),
            read: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("ts"),
                size: 16,
                usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }),
            period_ns: queue.get_timestamp_period(),
        });
        Some(Gpu {
            device,
            queue,
            pipeline,
            bufs: Default::default(),
            ts,
            prof: Default::default(),
            name: format!("{} ({:?})", info.name, info.backend),
        })
    }

    fn alloc(&self, cap: usize) -> Bufs {
        let dev = &self.device;
        let buf = |label: &str, size: usize, usage: wgpu::BufferUsages| {
            dev.create_buffer(&wgpu::BufferDescriptor { label: Some(label), size: size as u64, usage, mapped_at_creation: false })
        };
        let st = wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST;
        let params = buf("params", std::mem::size_of::<Params>(), wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST);
        let boxes = buf("boxes", MAX_BOXES * std::mem::size_of::<GBox>(), st);
        let lights = buf("lights", MAX_LIGHTS * std::mem::size_of::<GLight>(), st);
        let tasks = buf("tasks", cap * std::mem::size_of::<GTask>(), st);
        let sizes = [cap * 16, cap * 16, cap * MASKS * 4, cap * REGION_WORDS * 4];
        let outs: Vec<wgpu::Buffer> =
            sizes.iter().map(|&z| buf("out", z, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC)).collect();
        let read: Vec<wgpu::Buffer> = sizes
            .iter()
            .map(|&z| buf("read", z, wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST))
            .collect();
        let bind = dev.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("trace"),
            layout: &self.pipeline.get_bind_group_layout(0),
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: params.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: boxes.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: lights.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 3, resource: tasks.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 4, resource: outs[0].as_entire_binding() },
                wgpu::BindGroupEntry { binding: 5, resource: outs[1].as_entire_binding() },
                wgpu::BindGroupEntry { binding: 6, resource: outs[2].as_entire_binding() },
                wgpu::BindGroupEntry { binding: 7, resource: outs[3].as_entire_binding() },
            ],
        });
        Bufs { cap, params, boxes, lights, tasks, outs, read, bind }
    }

    pub fn trace(&self, scene: &Scene, reqs: &[Req], o: &Opts) -> Traced {
        let mut out = Traced::default();
        for chunk in reqs.chunks(MAX_BATCH) {
            if o.spp <= SPP_CHUNK {
                self.dispatch(scene, chunk, o, o.spp, 0, &mut out, 1.0);
                continue;
            }
            // beaucoup de chemins par requête (références) : plusieurs dispatchs, graines distinctes
            let mut part = Traced::default();
            for pass in 0..o.spp.div_ceil(SPP_CHUNK) {
                let spp = SPP_CHUNK.min(o.spp - pass * SPP_CHUNK);
                let mut p = Traced::default();
                self.dispatch(scene, chunk, o, spp, pass as u64 + 1, &mut p, spp as f64 / o.spp as f64);
                if part.l.is_empty() {
                    part = p;
                } else {
                    for i in 0..chunk.len() {
                        part.l[i] += p.l[i];
                        part.dl[i] += p.dl[i];
                    }
                }
            }
            out.l.extend(part.l);
            out.dl.extend(part.dl);
            out.recs.extend(part.recs);
        }
        out
    }

    /// Trace un lot et ajoute ses résultats (pondérés par `w`) à `out`.
    #[allow(clippy::too_many_arguments)]
    fn dispatch(&self, scene: &Scene, reqs: &[Req], o: &Opts, spp: u32, pass: u64, out: &mut Traced, w: f64) {
        let n = reqs.len();
        if n == 0 {
            return;
        }
        let dev = &self.device;
        let clock = std::time::Instant::now();
        let ms = |c: &std::time::Instant| c.elapsed().as_secs_f64() * 1e3;
        let mut prof = Prof { calls: 1, ..Default::default() };
        assert!(scene.objects.len() <= MAX_BOXES && scene.lights.len() <= MAX_LIGHTS);
        let (boxes, lights) = scene_to_gpu(scene, o.toggle_w);
        let tasks: Vec<GTask> = reqs
            .iter()
            .map(|r| {
                let s = if pass == 0 { r.seed } else { mix64(r.seed ^ pass.wrapping_mul(0x9e3779b97f4a7c15)) };
                GTask {
                    p: f3(r.p),
                    seed_lo: s as u32,
                    n: f3(r.n),
                    seed_hi: (s >> 32) as u32,
                    albedo: f3(r.albedo),
                    p0: 0,
                }
            })
            .collect();
        let params = Params {
            n_tasks: n as u32,
            n_boxes: boxes.len() as u32,
            n_lights: lights.len() as u32,
            flags: o.union as u32 | (o.record as u32) << 1,
            spp,
            pad: [0; 3],
        };
        prof.build = ms(&clock);
        let clock = std::time::Instant::now();
        let mut guard = self.bufs.lock().unwrap();
        if guard.as_ref().is_none_or(|b| b.cap < n) {
            *guard = Some(self.alloc(n.next_power_of_two().max(4096)));
        }
        let bufs = guard.as_ref().unwrap();
        let q = &self.queue;
        q.write_buffer(&bufs.params, 0, bytemuck::bytes_of(&params));
        q.write_buffer(&bufs.boxes, 0, bytemuck::cast_slice(&boxes));
        q.write_buffer(&bufs.lights, 0, bytemuck::cast_slice(&lights));
        q.write_buffer(&bufs.tasks, 0, bytemuck::cast_slice(&tasks));
        // seules les plages utiles sont copiées puis relues
        let sizes = [
            (n * 16) as u64,
            (n * 16) as u64,
            if o.record { (n * MASKS * 4) as u64 } else { 0 },
            if o.record { (n * REGION_WORDS * 4) as u64 } else { 0 },
        ];
        prof.upload = ms(&clock);
        let clock = std::time::Instant::now();
        let mut enc = dev.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("trace") });
        {
            let timestamp_writes = self.ts.as_ref().map(|t| wgpu::ComputePassTimestampWrites {
                query_set: &t.set,
                beginning_of_pass_write_index: Some(0),
                end_of_pass_write_index: Some(1),
            });
            let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("trace"), timestamp_writes });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, &bufs.bind, &[]);
            pass.dispatch_workgroups((n as u32).div_ceil(WG), 1, 1);
        }
        for k in 0..4 {
            if sizes[k] > 0 {
                enc.copy_buffer_to_buffer(&bufs.outs[k], 0, &bufs.read[k], 0, sizes[k]);
            }
        }
        if let Some(t) = &self.ts {
            enc.resolve_query_set(&t.set, 0..2, &t.resolve, 0);
            enc.copy_buffer_to_buffer(&t.resolve, 0, &t.read, 0, 16);
        }
        q.submit(Some(enc.finish()));
        for k in 0..4 {
            if sizes[k] > 0 {
                bufs.read[k].slice(..sizes[k]).map_async(wgpu::MapMode::Read, |r| r.expect("lecture GPU"));
            }
        }
        if let Some(t) = &self.ts {
            t.read.slice(..).map_async(wgpu::MapMode::Read, |r| r.expect("lecture timestamps"));
        }
        dev.poll(wgpu::Maintain::Wait);
        prof.wait = ms(&clock);
        if let Some(t) = &self.ts {
            let v: Vec<u64> = bytemuck::cast_slice::<u8, u64>(&t.read.slice(..).get_mapped_range()).to_vec();
            t.read.unmap();
            prof.kernel = v[1].saturating_sub(v[0]) as f64 * t.period_ns as f64 * 1e-6;
        }
        let clock = std::time::Instant::now();
        let data: Vec<Vec<u32>> = (0..4)
            .map(|k| {
                if sizes[k] == 0 {
                    return Vec::new();
                }
                let v = bytemuck::cast_slice::<u8, u32>(&bufs.read[k].slice(..sizes[k]).get_mapped_range()).to_vec();
                bufs.read[k].unmap();
                v
            })
            .collect();
        prof.read = ms(&clock);
        let clock = std::time::Instant::now();
        let fl = |x: u32| f32::from_bits(x) as f64 * w;
        for i in 0..n {
            out.l.push(v3(fl(data[0][4 * i]), fl(data[0][4 * i + 1]), fl(data[0][4 * i + 2])));
            out.dl.push(v3(fl(data[1][4 * i]), fl(data[1][4 * i + 1]), fl(data[1][4 * i + 2])));
            if o.record {
                let m = &data[2][MASKS * i..MASKS * (i + 1)];
                let rw = &data[3][REGION_WORDS * i..REGION_WORDS * (i + 1)];
                let mut regions = Bits::default();
                for k in 0..WORDS {
                    regions.0[k] = rw[2 * k] as u64 | (rw[2 * k + 1] as u64) << 32;
                }
                out.recs.push(DepRec {
                    regions,
                    lights: m[0] as u64,
                    objs: m[1] as u128 | (m[2] as u128) << 32 | (m[3] as u128) << 64 | (m[4] as u128) << 96,
                    vsup: m[5] as u64 | (m[6] as u64) << 32,
                });
            }
        }
        prof.convert = ms(&clock);
        let mut p = self.prof.lock().unwrap();
        p.calls += 1;
        p.build += prof.build;
        p.upload += prof.upload;
        p.wait += prof.wait;
        p.kernel += prof.kernel;
        p.read += prof.read;
        p.convert += prof.convert;
    }
}

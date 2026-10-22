//! Scènes en triangles et structures d'accélération matérielles (ray query Vulkan via wgpu).
use crate::gpu::Gpu;
use crate::math::{v3, V3};
use crate::scene::{Aabb, Light};

/// Maillage indexé, un albédo par maillage.
#[derive(Clone)]
pub struct Mesh {
    pub pos: Vec<[f32; 3]>,
    pub idx: Vec<u32>,
    pub albedo: [f32; 3],
}

impl Mesh {
    fn new(albedo: [f32; 3]) -> Mesh {
        Mesh { pos: Vec::new(), idx: Vec::new(), albedo }
    }
    /// Quadrilatère a, b, c, d (dans l'ordre du contour).
    fn quad(&mut self, a: V3, b: V3, c: V3, d: V3) {
        let i = self.pos.len() as u32;
        for p in [a, b, c, d] {
            self.pos.push([p.x as f32, p.y as f32, p.z as f32]);
        }
        self.idx.extend_from_slice(&[i, i + 1, i + 2, i, i + 2, i + 3]);
    }
    /// Boîte alignée (6 faces).
    pub fn cuboid(albedo: [f32; 3], b: &Aabb) -> Mesh {
        let mut m = Mesh::new(albedo);
        let (l, h) = (b.min, b.max);
        let p = |x: f64, y: f64, z: f64| v3(x, y, z);
        m.quad(p(l.x, l.y, l.z), p(h.x, l.y, l.z), p(h.x, l.y, h.z), p(l.x, l.y, h.z));
        m.quad(p(l.x, h.y, l.z), p(l.x, h.y, h.z), p(h.x, h.y, h.z), p(h.x, h.y, l.z));
        m.quad(p(l.x, l.y, l.z), p(l.x, h.y, l.z), p(h.x, h.y, l.z), p(h.x, l.y, l.z));
        m.quad(p(l.x, l.y, h.z), p(h.x, l.y, h.z), p(h.x, h.y, h.z), p(l.x, h.y, h.z));
        m.quad(p(l.x, l.y, l.z), p(l.x, l.y, h.z), p(l.x, h.y, h.z), p(l.x, h.y, l.z));
        m.quad(p(h.x, l.y, l.z), p(h.x, h.y, l.z), p(h.x, h.y, h.z), p(h.x, l.y, h.z));
        m
    }
}

/// Instance : maillage + translation (les objets mobiles sont des instances déplacées).
#[derive(Clone, Copy)]
pub struct Instance {
    pub mesh: usize,
    pub offset: V3,
}

pub struct TriScene {
    pub meshes: Vec<Mesh>,
    pub instances: Vec<Instance>,
    pub lights: Vec<Light>,
    pub bounds: Aabb,
}

impl TriScene {
    pub fn instance_aabb(&self, i: usize) -> Aabb {
        let inst = self.instances[i];
        let m = &self.meshes[inst.mesh];
        let mut b = Aabb { min: V3::splat(f64::INFINITY), max: V3::splat(f64::NEG_INFINITY) };
        for p in &m.pos {
            let q = v3(p[0] as f64, p[1] as f64, p[2] as f64) + inst.offset;
            b = Aabb { min: b.min.min(q), max: b.max.max(q) };
        }
        b
    }
    pub fn triangles(&self) -> usize {
        self.instances.iter().map(|i| self.meshes[i.mesh].idx.len() / 3).sum()
    }
}

/// Cornell box (5,5 m), boîte haute fixe, boîte basse mobile (instance 1), plafonnier (lampe 0)
/// et une seconde lampe chaude à l'avant gauche (lampe 1) : mêmes événements que la séquence
/// des pièces (boîte qui bouge, lampe qui s'allume, lampe qui s'éteint).
pub fn cornell(step: u32, light_on: bool, lamp2_on: bool) -> TriScene {
    let white = [0.73, 0.73, 0.73];
    let red = [0.65, 0.05, 0.05];
    let green = [0.12, 0.45, 0.15];
    let s = 5.5;
    let mut room = Mesh::new(white);
    let p = |x: f64, y: f64, z: f64| v3(x, y, z);
    room.quad(p(0.0, 0.0, 0.0), p(0.0, 0.0, s), p(s, 0.0, s), p(s, 0.0, 0.0)); // sol
    room.quad(p(0.0, s, 0.0), p(s, s, 0.0), p(s, s, s), p(0.0, s, s)); // plafond
    room.quad(p(0.0, 0.0, 0.0), p(s, 0.0, 0.0), p(s, s, 0.0), p(0.0, s, 0.0)); // fond
    let mut left = Mesh::new(red);
    left.quad(p(0.0, 0.0, 0.0), p(0.0, s, 0.0), p(0.0, s, s), p(0.0, 0.0, s));
    let mut right = Mesh::new(green);
    right.quad(p(s, 0.0, 0.0), p(s, 0.0, s), p(s, s, s), p(s, s, 0.0));
    let tall = Mesh::cuboid(white, &Aabb { min: v3(0.9, 0.0, 1.0), max: v3(2.4, 3.3, 2.5) });
    let short = Mesh::cuboid(white, &Aabb { min: v3(0.0, 0.0, 0.0), max: v3(1.5, 1.65, 1.5) });
    let meshes = vec![room, left, right, tall, short];
    let shift = v3(3.3 - step as f64 * 0.1, 0.0, 2.6);
    let mut instances: Vec<Instance> = (0..4).map(|m| Instance { mesh: m, offset: V3::default() }).collect();
    instances.insert(1, Instance { mesh: 4, offset: shift }); // instance 1 = boîte mobile
    let lights = vec![
        Light { center: v3(2.75, s - 0.01, 2.75), hx: 0.65, hz: 0.55, emission: V3::splat(15.0), on: light_on },
        Light { center: v3(1.0, s - 0.01, 4.4), hx: 0.35, hz: 0.35, emission: v3(40.0, 30.0, 18.0), on: lamp2_on },
    ];
    TriScene { meshes, instances, lights, bounds: Aabb { min: V3::splat(-0.1), max: V3::splat(s + 0.1) } }
}

/// Structures d'accélération GPU d'une scène : une BLAS par maillage, une TLAS d'instances.
pub struct Accel {
    /// deux TLAS alternées : celle de la frame courante et celle de la précédente
    pub tlas: [wgpu::TlasPackage; 2],
    pub blas: Vec<wgpu::Blas>,
    /// sommets (vec4 alignés) et indices de tous les maillages, pour les normales dans le shader
    pub verts: wgpu::Buffer,
    pub idx: wgpu::Buffer,
    /// par maillage : décalage des sommets, des indices, albédo
    pub mesh_table: wgpu::Buffer,
    vbufs: Vec<wgpu::Buffer>,
    ibufs: Vec<wgpu::Buffer>,
    sizes: Vec<wgpu::BlasTriangleGeometrySizeDescriptor>,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct GMesh {
    pub first_vertex: u32,
    pub first_index: u32,
    pub pad: [u32; 2],
    pub albedo: [f32; 3],
    pub pad2: f32,
}

fn transform(o: V3) -> [f32; 12] {
    [1.0, 0.0, 0.0, o.x as f32, 0.0, 1.0, 0.0, o.y as f32, 0.0, 0.0, 1.0, o.z as f32]
}

impl Accel {
    pub fn new(gpu: &Gpu, scene: &TriScene) -> Accel {
        use wgpu::util::DeviceExt;
        let dev = &gpu.device;
        let mut vbufs = Vec::new();
        let mut ibufs = Vec::new();
        let mut sizes = Vec::new();
        let mut blas = Vec::new();
        let (mut all_v, mut all_i, mut table) = (Vec::<[f32; 4]>::new(), Vec::<u32>::new(), Vec::new());
        for m in &scene.meshes {
            table.push(GMesh {
                first_vertex: all_v.len() as u32,
                first_index: all_i.len() as u32,
                pad: [0; 2],
                albedo: m.albedo,
                pad2: 0.0,
            });
            all_v.extend(m.pos.iter().map(|p| [p[0], p[1], p[2], 0.0]));
            all_i.extend_from_slice(&m.idx);
            vbufs.push(dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("blas v"),
                contents: bytemuck::cast_slice(&m.pos),
                usage: wgpu::BufferUsages::BLAS_INPUT,
            }));
            ibufs.push(dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("blas i"),
                contents: bytemuck::cast_slice(&m.idx),
                usage: wgpu::BufferUsages::BLAS_INPUT,
            }));
            let size = wgpu::BlasTriangleGeometrySizeDescriptor {
                vertex_format: wgpu::VertexFormat::Float32x3,
                vertex_count: m.pos.len() as u32,
                index_format: Some(wgpu::IndexFormat::Uint32),
                index_count: Some(m.idx.len() as u32),
                flags: wgpu::AccelerationStructureGeometryFlags::OPAQUE,
            };
            blas.push(dev.create_blas(
                &wgpu::CreateBlasDescriptor {
                    label: Some("blas"),
                    flags: wgpu::AccelerationStructureFlags::PREFER_FAST_TRACE,
                    update_mode: wgpu::AccelerationStructureUpdateMode::Build,
                },
                wgpu::BlasGeometrySizeDescriptors::Triangles { descriptors: vec![size.clone()] },
            ));
            sizes.push(size);
        }
        let tlas = || {
            wgpu::TlasPackage::new(dev.create_tlas(&wgpu::CreateTlasDescriptor {
                label: Some("tlas"),
                max_instances: scene.instances.len() as u32,
                flags: wgpu::AccelerationStructureFlags::PREFER_FAST_TRACE,
                update_mode: wgpu::AccelerationStructureUpdateMode::Build,
            }))
        };
        let st = wgpu::BufferUsages::STORAGE;
        let mut a = Accel {
            tlas: [tlas(), tlas()],
            verts: dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("verts"),
                contents: bytemuck::cast_slice(&all_v),
                usage: st,
            }),
            idx: dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("idx"),
                contents: bytemuck::cast_slice(&all_i),
                usage: st,
            }),
            mesh_table: dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("meshes"),
                contents: bytemuck::cast_slice(&table),
                usage: st,
            }),
            blas,
            vbufs,
            ibufs,
            sizes,
        };
        a.build(gpu, scene, 0, true);
        a.build(gpu, scene, 1, false);
        a
    }

    /// Reconstruit la TLAS `k` avec les positions courantes des instances.
    pub fn build_tlas(&mut self, gpu: &Gpu, scene: &TriScene, k: usize) {
        self.build(gpu, scene, k, false);
    }

    fn build(&mut self, gpu: &Gpu, scene: &TriScene, k: usize, with_blas: bool) {
        for (i, inst) in scene.instances.iter().enumerate() {
            self.tlas[k][i] = Some(wgpu::TlasInstance::new(&self.blas[inst.mesh], transform(inst.offset), inst.mesh as u32, 0xff));
        }
        let entries: Vec<wgpu::BlasBuildEntry> = if with_blas {
            (0..self.blas.len())
                .map(|m| wgpu::BlasBuildEntry {
                    blas: &self.blas[m],
                    geometry: wgpu::BlasGeometries::TriangleGeometries(vec![wgpu::BlasTriangleGeometry {
                        size: &self.sizes[m],
                        vertex_buffer: &self.vbufs[m],
                        first_vertex: 0,
                        vertex_stride: 12,
                        index_buffer: Some(&self.ibufs[m]),
                        first_index: Some(0),
                        transform_buffer: None,
                        transform_buffer_offset: None,
                    }]),
                })
                .collect()
        } else {
            Vec::new()
        };
        let mut enc = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("as build") });
        enc.build_acceleration_structures(entries.iter(), std::iter::once(&self.tlas[k]));
        gpu.queue.submit(Some(enc.finish()));
    }
}

/// `krc rtsmoke` : normales d'une Cornell box vues par ray query matériel (validation de l'API).
pub fn smoke(gpu: &Gpu) {
    use wgpu::util::DeviceExt;
    let scene = cornell(0, true, false);
    let accel = Accel::new(gpu, &scene);
    let (w, h) = (512u32, 512u32);
    let dev = &gpu.device;
    let module = dev.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("rtsmoke"),
        source: wgpu::ShaderSource::Wgsl(
            r#"
struct GMesh { first_vertex: u32, first_index: u32, p0: u32, p1: u32, albedo: vec3<f32>, p2: f32 };
@group(0) @binding(0) var acc: acceleration_structure;
@group(0) @binding(1) var<storage, read> verts: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> idx: array<u32>;
@group(0) @binding(3) var<storage, read> meshes: array<GMesh>;
@group(0) @binding(4) var<storage, read_write> img: array<vec4<f32>>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= 512u || gid.y >= 512u) { return; }
    let uv = (vec2<f32>(gid.xy) + 0.5) / 512.0 * 2.0 - 1.0;
    let o = vec3<f32>(2.75, 2.75, 13.0);
    let d = normalize(vec3<f32>(uv.x * 0.36, -uv.y * 0.36, -1.0));
    var rq: ray_query;
    rayQueryInitialize(&rq, acc, RayDesc(0u, 0xffu, 0.001, 1e30, o, d));
    rayQueryProceed(&rq);
    let hit = rayQueryGetCommittedIntersection(&rq);
    var c = vec3<f32>(0.0);
    if (hit.kind != RAY_QUERY_INTERSECTION_NONE) {
        let m = meshes[hit.instance_custom_index];
        let t = m.first_index + 3u * hit.primitive_index;
        let a = verts[m.first_vertex + idx[t]].xyz;
        let b = verts[m.first_vertex + idx[t + 1u]].xyz;
        let e = verts[m.first_vertex + idx[t + 2u]].xyz;
        var n = normalize((hit.object_to_world * vec4<f32>(cross(b - a, e - a), 0.0)).xyz);
        if (dot(n, d) > 0.0) { n = -n; }
        c = m.albedo * (0.35 + 0.65 * max(dot(n, normalize(vec3<f32>(0.3, 0.8, 0.5))), 0.0));
    }
    img[gid.y * 512u + gid.x] = vec4<f32>(c, 1.0);
}
"#
            .into(),
        ),
    });
    let pipe = dev.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("rtsmoke"),
        layout: None,
        module: &module,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });
    let img = dev.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: (w * h * 16) as u64,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let read = dev.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: (w * h * 16) as u64,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let bind = dev.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipe.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: accel.tlas[0].as_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: accel.verts.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 2, resource: accel.idx.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 3, resource: accel.mesh_table.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 4, resource: img.as_entire_binding() },
        ],
    });
    let _ = dev.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: None, contents: &[0u8; 4], usage: wgpu::BufferUsages::STORAGE });
    let mut enc = dev.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipe);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(w / 8, h / 8, 1);
    }
    enc.copy_buffer_to_buffer(&img, 0, &read, 0, (w * h * 16) as u64);
    gpu.queue.submit(Some(enc.finish()));
    read.slice(..).map_async(wgpu::MapMode::Read, |r| r.unwrap());
    dev.poll(wgpu::Maintain::Wait);
    let data: Vec<f32> = bytemuck::cast_slice(&read.slice(..).get_mapped_range()).to_vec();
    let px: Vec<V3> = (0..(w * h) as usize).map(|k| v3(data[4 * k] as f64, data[4 * k + 1] as f64, data[4 * k + 2] as f64)).collect();
    std::fs::create_dir_all("results/rt").unwrap();
    crate::image::save("results/rt/smoke_cornell.png", w as usize, h as usize, &px);
    println!("ray query OK : {} triangles, image results/rt/smoke_cornell.png", scene.triangles());
}

/// Bloc de géométrie du shader pour les triangles : ray query matériel sur la TLAS courante
/// (binding 1) ou précédente (binding 3) ; normale du triangle, albédo du maillage.
pub const TRI_GEOMETRY: &str = r#"// <geometrie> triangles : ray query matériel
struct GMesh { first_vertex: u32, first_index: u32, p0: u32, p1: u32, albedo: vec3<f32>, p2: f32 };
@group(0) @binding(1) var acc_cur: acceleration_structure;
@group(0) @binding(3) var acc_prev: acceleration_structure;
@group(0) @binding(18) var<storage, read> verts: array<vec4<f32>>;
@group(0) @binding(19) var<storage, read> tidx: array<u32>;
@group(0) @binding(20) var<storage, read> meshes: array<GMesh>;

fn rq_trace(o: vec3<f32>, d: vec3<f32>, tmax: f32, flags: u32) -> RayIntersection {
    var q: ray_query;
    let desc = RayDesc(flags, 0xffu, EPS, tmax, o, d);
    if (sel == 0u) { rayQueryInitialize(&q, acc_cur, desc); } else { rayQueryInitialize(&q, acc_prev, desc); }
    rayQueryProceed(&q);
    return rayQueryGetCommittedIntersection(&q);
}

fn scene_hit(o: vec3<f32>, d: vec3<f32>) -> Hit {
    var h: Hit;
    h.found = false;
    h.t = 1e30;
    h.light = -1;
    let r = rq_trace(o, d, 1e30, 0u);
    if (r.kind != RAY_QUERY_INTERSECTION_NONE) {
        let m = meshes[r.instance_custom_index];
        let ti = m.first_index + 3u * r.primitive_index;
        let a = verts[m.first_vertex + tidx[ti]].xyz;
        let b = verts[m.first_vertex + tidx[ti + 1u]].xyz;
        let c = verts[m.first_vertex + tidx[ti + 2u]].xyz;
        var n = normalize((r.object_to_world * vec4<f32>(cross(b - a, c - a), 0.0)).xyz);
        if (dot(n, d) > 0.0) { n = -n; }
        h.found = true;
        h.t = r.t;
        h.n = n;
        h.albedo = m.albedo;
    }
    return h;
}

fn occluder(o: vec3<f32>, d: vec3<f32>, tmax: f32) -> Occ {
    var r: Occ;
    let i = rq_trace(o, d, tmax, RAY_FLAG_TERMINATE_ON_FIRST_HIT);
    r.found = i.kind != RAY_QUERY_INTERSECTION_NONE;
    r.t = select(tmax, i.t, r.found);
    return r;
}
"#;

/// Vue de la Cornell box (carrée).
pub fn cornell_view(size: usize) -> crate::gpu_krc::View {
    crate::gpu_krc::View {
        w: size,
        h: size,
        eye: v3(2.75, 2.75, 13.0),
        fwd: v3(0.0, 0.0, -1.0),
        right: v3(1.0, 0.0, 0.0),
        up: v3(0.0, 1.0, 0.0),
        th: 0.36,
        tv: 0.36,
    }
}

pub fn cornell_at(frame: u32) -> TriScene {
    let s = crate::scene::state_at(frame);
    cornell(s.box_step, s.l0_on, s.l2_on)
}

/// Une scène en triangles pour les bancs : nom, vue, scène à chaque frame de la séquence.
pub struct TriSetup {
    pub name: String,
    pub view: crate::gpu_krc::View,
    pub at: fn(u32) -> TriScene,
    pub ref_spp: u32,
    /// caméra mobile : (œil, point visé) à la frame t ; None = caméra fixe `view`
    pub cam: Option<fn(u32) -> (V3, V3)>,
}

impl TriSetup {
    /// Vue de la frame t (même résolution et même champ que `view`).
    pub fn view_at(&self, t: u32) -> crate::gpu_krc::View {
        let mut v = self.view;
        if let Some(cam) = self.cam {
            let (eye, target) = cam(t);
            v.eye = eye;
            v.fwd = (target - eye).norm();
            v.right = v.fwd.cross(v3(0.0, 1.0, 0.0)).norm();
            v.up = v.right.cross(v.fwd);
        }
        v
    }

    /// Nom de la référence de la frame t : l'état de la scène si la caméra est fixe, sinon la frame.
    pub fn ref_key(&self, t: u32) -> String {
        match self.cam {
            Some(_) => format!("f{t:03}"),
            None => crate::scene::state_at(t).id(),
        }
    }
}

pub fn cornell_setup(size: usize) -> TriSetup {
    TriSetup { name: format!("cornell_{size}"), view: cornell_view(size), at: cornell_at, ref_spp: 4096, cam: None }
}

/// Cornell box, caméra mobile : travelling avant (z 13 → 9) avec un balancement latéral.
pub fn cornell_cam_setup(size: usize) -> TriSetup {
    fn cam(t: u32) -> (V3, V3) {
        let u = t as f64 / 119.0;
        let a = std::f64::consts::PI * t as f64 / 60.0;
        (v3(2.75 + 1.5 * a.sin(), 2.75 + 0.4 * (2.0 * a).sin(), 13.0 - 4.0 * u), v3(2.75, 2.5, 2.0))
    }
    TriSetup { name: format!("cornell_cam_{size}"), cam: Some(cam), ..cornell_setup(size) }
}

/// relMSE d'une frame sur les pixels de surface : brute, et sans le 0,1 % de pixels les plus
/// fautifs (pratique courante du path guiding : une luciole isolée ne doit pas décider seule).
/// Troisième valeur : luminance totale image / référence, pour voir un biais que la relMSE cache
/// (avec y² + 0,01 au dénominateur, une scène sombre récompense une image trop sombre).
pub fn frame_err(img: &[V3], kinds: &[u8], r: &[V3]) -> (f64, f64, f64) {
    let mut px: Vec<f64> = Vec::with_capacity(img.len());
    let (mut li, mut lr) = (0.0, 0.0);
    for i in 0..img.len() {
        if kinds[i] != 2 {
            continue;
        }
        li += img[i].lum();
        lr += r[i].lum();
        let mut e = 0.0;
        for c in 0..3 {
            let (x, y) = (img[i].get(c), r[i].get(c));
            e += (x - y) * (x - y) / (y * y + 0.01);
        }
        px.push(e / 3.0);
    }
    let n = px.len().max(1) as f64;
    let raw = px.iter().sum::<f64>() / n;
    let keep = px.len() - px.len() / 1000;
    if keep < px.len() {
        px.select_nth_unstable_by(keep, |a, b| a.total_cmp(b));
    }
    let trim = px[..keep].iter().sum::<f64>() / keep.max(1) as f64;
    (raw, trim, li / lr.max(1e-12))
}

/// `krc cornell|sponza` : la séquence de 120 frames (objet qui bouge, lampe allumée, lampe
/// éteinte) sur une scène en triangles, ray tracing matériel, pipeline entièrement GPU.
pub fn run_tri(gpu: &Gpu, seeds: u64, setup: &TriSetup) {
    use crate::gpu_krc::GpuKrc;
    use crate::grid::Grid;
    use crate::render::{load_raw, save_raw};
    use crate::scene::FRAMES;
    use std::collections::HashMap;
    let view = setup.view;
    let at = setup.at;
    let base = at(0);
    let grid = Grid::fit(&base.bounds);
    eprintln!(
        "grille : régions {:.3} m, {:?} ({} régions), super-régions {:?}",
        grid.region,
        grid.dims,
        grid.dims.iter().product::<i64>(),
        grid.sdims()
    );
    let dir = format!("results/ref_{}", setup.name);
    std::fs::create_dir_all(&dir).unwrap();
    let mut refs: HashMap<String, Vec<V3>> = HashMap::new();
    let mut rk = GpuKrc::with(gpu, view, grid, Some(&base), false, 0);
    for f in 0..FRAMES {
        let id = setup.ref_key(f);
        if refs.contains_key(&id) {
            continue;
        }
        let path = format!("{dir}/{id}.bin");
        let img = load_raw(&path, view.len()).unwrap_or_else(|| {
            let t = std::time::Instant::now();
            rk.set_view(setup.view_at(f));
            let (img, _) = rk.reference_tri(&at(f), setup.ref_spp);
            save_raw(&path, &img);
            crate::image::save(&format!("{dir}/{id}.png"), view.w, view.h, &img);
            eprintln!("référence {id} : {:.1} s", t.elapsed().as_secs_f64());
            img
        });
        refs.insert(id, img);
    }
    let phases: [(&str, u32, u32); 6] =
        [("tout", 0, 120), ("statique", 30, 40), ("boite", 40, 64), ("apres_boite", 64, 80), ("lampe_on", 80, 100), ("lampe_off", 100, 120)];
    let mut summary = String::from("method,phase,relmse_mean,relmse_std,gpu_ms,paths_per_frame,trim_mean,trim_std,lum_ratio\n");
    for (name, sharc) in [("tri_krc", false), ("tri_sharc", true)] {
        let out = format!("results/{}/{name}", setup.name);
        std::fs::create_dir_all(&out).unwrap();
        let mut runs: Vec<Vec<(f64, f64, u32, f64, f64)>> = Vec::new();
        for seed in 0..seeds {
            let mut k = GpuKrc::with(gpu, view, grid, Some(&base), sharc, 1 + seed as u32);
            let mut prev = at(0);
            let mut rows = Vec::new();
            let mut csv = String::from("frame,relmse,gpu_ms,paths,suspects,delta_paths,relmse_trim,lum_ratio,cells_live,evicted\n");
            for t in 0..FRAMES {
                let scene = at(t);
                k.set_view(setup.view_at(t));
                let f = k.frame_tri(&scene, &prev, t);
                assert_eq!(f.overflow, 0);
                let (rel, trim, lr) = frame_err(&f.image, &f.kinds, &refs[&setup.ref_key(t)]);
                let paths = f.regular_paths + f.delta_paths + f.probe_rays as u32;
                csv += &format!(
                    "{t},{rel:.6e},{:.4},{paths},{},{},{trim:.6e},{lr:.4},{},{}\n",
                    f.gpu_ms(),
                    f.suspects,
                    f.delta_paths,
                    f.cells - f.free,
                    f.evicted
                );
                if seed == 0 && crate::methods::SAVE_FRAMES.contains(&t) {
                    crate::image::save(&format!("{out}/f{t:03}.png"), view.w, view.h, &f.image);
                }
                rows.push((rel, f.gpu_ms(), paths, trim, lr));
                prev = scene;
            }
            let file = if seed == 0 { "metrics.csv".to_string() } else { format!("metrics_s{seed}.csv") };
            std::fs::write(format!("{out}/{file}"), csv).unwrap();
            runs.push(rows);
        }
        let mut line = format!("{name:<10}");
        let mut line_t = format!("{:<10}", format!("{name}*"));
        let stat = |v: Vec<f64>| {
            let m = v.iter().sum::<f64>() / v.len() as f64;
            (m, (v.iter().map(|x| (x - m) * (x - m)).sum::<f64>() / (v.len().max(2) - 1) as f64).sqrt())
        };
        for (phase, a, b) in phases {
            let (m, sd) =
                stat(runs.iter().map(|r| r[a as usize..b as usize].iter().map(|x| x.0).sum::<f64>() / (b - a) as f64).collect());
            let (mt, sdt) =
                stat(runs.iter().map(|r| r[a as usize..b as usize].iter().map(|x| x.3).sum::<f64>() / (b - a) as f64).collect());
            let (lr, _) =
                stat(runs.iter().map(|r| r[a as usize..b as usize].iter().map(|x| x.4).sum::<f64>() / (b - a) as f64).collect());
            let ms: f64 = runs.iter().map(|r| r[a as usize..b as usize].iter().map(|x| x.1).sum::<f64>() / (b - a) as f64).sum::<f64>()
                / runs.len() as f64;
            let paths: f64 = runs
                .iter()
                .map(|r| r[a as usize..b as usize].iter().map(|x| x.2 as f64).sum::<f64>() / (b - a) as f64)
                .sum::<f64>()
                / runs.len() as f64;
            summary += &format!("{name},{phase},{m:.6e},{sd:.6e},{ms:.4},{paths:.0},{mt:.6e},{sdt:.6e},{lr:.4}\n");
            line += &format!(" | {phase} {m:.2e}±{sd:.0e} {ms:.2}ms");
            line_t += &format!(" | {phase} {mt:.2e}±{sdt:.0e} lum {lr:.3}");
        }
        println!("{line}");
        println!("{line_t}");
    }
    std::fs::write(format!("results/{}/summary.csv", setup.name), summary).unwrap();
}

/// `krc pareto [taille]` : erreur contre temps GPU, KRC et SHaRC, pour plusieurs budgets
/// (chemins par pixel et par frame). Erreur : un passage avec relecture d'image ; temps : deux
/// passages sans relecture (chauffe, puis mesure ; médiane par frame).
pub fn pareto(gpu: &Gpu, setup: &TriSetup) {
    use crate::gpu_krc::GpuKrc;
    use crate::grid::Grid;
    use crate::render::load_raw;
    use crate::scene::FRAMES;
    let view = setup.view;
    let at = setup.at;
    let base = at(0);
    let grid = Grid::fit(&base.bounds);
    let dir = format!("results/ref_{}", setup.name);
    let refs: std::collections::HashMap<String, Vec<V3>> = (0..FRAMES)
        .map(|f| setup.ref_key(f))
        .map(|id| {
            let img = load_raw(&format!("{dir}/{id}.bin"), view.len()).expect("références manquantes : lancer d'abord la séquence");
            (id, img)
        })
        .collect();
    let mut csv = String::from("method,ppp,relmse,relmse_static,relmse_dynamic,ms_median,ms_mean,ms_p95,trim,trim_static,trim_dynamic\n");
    let variants = [
        ("tri_krc", false, false, vec![0.25f32, 0.5, 1.0, 2.0, 4.0]),
        ("tri_krc_amort", false, true, vec![0.25f32, 0.5, 1.0, 2.0, 4.0]),
        ("tri_sharc", true, false, vec![0.5f32, 1.0, 2.0, 4.0, 8.0]),
    ];
    for (name, sharc, amortize, ppps) in variants {
        for ppp in ppps {
            let mut k = GpuKrc::with(gpu, view, grid, Some(&base), sharc, 3);
            k.ppp = ppp;
            k.amortize = amortize;
            let mut prev = at(0);
            let (mut rel, mut rel_s, mut rel_d) = (Vec::new(), Vec::new(), Vec::new());
            let (mut tr, mut tr_s, mut tr_d) = (Vec::new(), Vec::new(), Vec::new());
            let mut ms = Vec::new();
            for pass in 0..3 {
                k.readback = pass == 0;
                for t in 0..FRAMES {
                    let scene = at(t);
                    k.set_view(setup.view_at(t));
                    let f = k.frame_tri(&scene, &prev, t);
                    prev = scene;
                    if pass == 0 {
                        let (e, et, _) = frame_err(&f.image, &f.kinds, &refs[&setup.ref_key(t)]);
                        rel.push(e);
                        tr.push(et);
                        if (30..40).contains(&t) {
                            rel_s.push(e);
                            tr_s.push(et);
                        }
                        if t >= 40 {
                            rel_d.push(e);
                            tr_d.push(et);
                        }
                    }
                    if pass == 2 {
                        ms.push(f.gpu_ms());
                    }
                }
            }
            let mean = |v: &Vec<f64>| v.iter().sum::<f64>() / v.len() as f64;
            ms.sort_by(|a, b| a.total_cmp(b));
            let (med, p95) = (ms[ms.len() / 2], ms[ms.len() * 95 / 100]);
            println!(
                "{name:<14} {ppp:>5} chemin/px : relMSE {:.3e} (statique {:.2e}, dynamique {:.2e}) | GPU médiane {med:.2} ms, moyenne {:.2} ms, p95 {p95:.2} ms",
                mean(&rel),
                mean(&rel_s),
                mean(&rel_d),
                mean(&ms)
            );
            println!(
                "{:<14} {ppp:>5} chemin/px : relMSE sans 0,1 % {:.3e} (statique {:.2e}, dynamique {:.2e})",
                format!("{name}*"),
                mean(&tr),
                mean(&tr_s),
                mean(&tr_d)
            );
            csv += &format!(
                "{name},{ppp},{:.6e},{:.6e},{:.6e},{med:.4},{:.4},{p95:.4},{:.6e},{:.6e},{:.6e}\n",
                mean(&rel),
                mean(&rel_s),
                mean(&rel_d),
                mean(&ms),
                mean(&tr),
                mean(&tr_s),
                mean(&tr_d)
            );
        }
    }
    std::fs::create_dir_all(format!("results/{}", setup.name)).unwrap();
    std::fs::write(format!("results/{}/pareto.csv", setup.name), csv).unwrap();
}

/// `krc spikes [taille]` : composition du temps GPU des frames d'événement (KRC, 1 chemin/px).
pub fn spikes(gpu: &Gpu, setup: &TriSetup) {
    use crate::gpu_krc::GpuKrc;
    use crate::grid::Grid;
    use crate::scene::FRAMES;
    let view = setup.view;
    let at = setup.at;
    let base = at(0);
    let mut k = GpuKrc::with(gpu, view, Grid::fit(&base.bounds), Some(&base), false, 3);
    k.readback = false;
    k.amortize = std::env::var("KRC_AMORT").is_ok();
    let mut prev = at(0);
    for pass in 0..2 {
        for t in 0..FRAMES {
            let scene = at(t);
            let f = k.frame_tri(&scene, &prev, t);
            prev = scene;
            if pass == 1 && [38u32, 40, 41, 80, 81, 82, 83, 100, 101, 102].contains(&t) {
                let mut p: Vec<String> = f.passes.iter().filter(|x| x.1 > 0.05).map(|(n, ms)| format!("{n} {ms:.2}")).collect();
                p.sort();
                println!(
                    "frame {t:>3} : {:.2} ms | suspects {} | en attente {} | chemins corrélés {} (retracés {}) | ordinaires {} | {}",
                    f.gpu_ms(),
                    f.suspects,
                    f.pending,
                    f.delta_paths,
                    f.delta_old,
                    f.regular_paths,
                    p.join(", ")
                );
            }
        }
    }
}

/// Géométrie de Sponza (Khronos glTF-Sample-Assets, CC-BY : Crytek / Frank Meinl), chargée une
/// fois : une primitive = un maillage ; albédo = couleur de base (les textures sont ignorées),
/// légèrement teintée par matériau pour distinguer les surfaces.
fn sponza_meshes() -> &'static (Vec<Mesh>, Aabb) {
    static CELL: std::sync::OnceLock<(Vec<Mesh>, Aabb)> = std::sync::OnceLock::new();
    CELL.get_or_init(|| {
        let dir = "assets/sponza";
        let gltf = gltf::Gltf::open(format!("{dir}/Sponza.gltf")).expect("assets/sponza/Sponza.gltf : lancer tools/fetch_sponza.sh");
        let bin = std::fs::read(format!("{dir}/Sponza.bin")).expect("Sponza.bin");
        let scale = 0.008f32;
        let mut meshes = Vec::new();
        let mut b = Aabb { min: V3::splat(f64::INFINITY), max: V3::splat(f64::NEG_INFINITY) };
        for mesh in gltf.meshes() {
            for prim in mesh.primitives() {
                let reader = prim.reader(|_| Some(&bin[..]));
                let pos: Vec<[f32; 3]> = reader.read_positions().unwrap().map(|p| [p[0] * scale, p[1] * scale, p[2] * scale]).collect();
                let idx: Vec<u32> = reader.read_indices().unwrap().into_u32().collect();
                let mat = prim.material();
                let c = mat.pbr_metallic_roughness().base_color_factor();
                let m = mat.index().unwrap_or(0) as u64;
                let h = crate::math::mix64(m ^ 0x5b0);
                let tint = |k: u32| 0.85 + 0.3 * ((h >> (8 * k)) & 0xff) as f32 / 255.0;
                let albedo = [(c[0] * tint(0)).min(0.9), (c[1] * tint(1)).min(0.9), (c[2] * tint(2)).min(0.9)];
                for p in &pos {
                    let q = v3(p[0] as f64, p[1] as f64, p[2] as f64);
                    b = Aabb { min: b.min.min(q), max: b.max.max(q) };
                }
                meshes.push(Mesh { pos, idx, albedo });
            }
        }
        (meshes, b.expand(0.1))
    })
}

/// Sponza éclairée par trois lampes suspendues dans l'atrium (lampe 0 : centrale, s'éteint) et une
/// lampe d'appoint chaude dans la galerie basse (lampe 1, s'allume) ; une boîte se déplace au sol.
pub fn sponza(step: u32, l0_on: bool, l2_on: bool) -> TriScene {
    let (meshes0, bounds) = sponza_meshes();
    let mut meshes = meshes0.clone();
    let mut instances: Vec<Instance> = (0..meshes.len()).map(|m| Instance { mesh: m, offset: V3::default() }).collect();
    meshes.push(Mesh::cuboid([0.25, 0.35, 0.8], &Aabb { min: v3(0.0, 0.0, 0.0), max: v3(1.4, 1.4, 1.4) }));
    let floor = 0.0;
    instances.insert(1, Instance { mesh: meshes.len() - 1, offset: v3(3.0 - step as f64 * 0.5, floor, -0.7) });
    let lamp = |x: f64, on: bool| Light { center: v3(x, 9.0, -0.3), hx: 0.8, hz: 0.8, emission: V3::splat(30.0), on };
    let lights = vec![
        lamp(0.0, l0_on),
        Light { center: v3(-6.0, 2.5, 4.0), hx: 0.4, hz: 0.4, emission: v3(60.0, 45.0, 27.0), on: l2_on },
        lamp(-8.5, true),
        lamp(8.5, true),
    ];
    TriScene { meshes, instances, lights, bounds: *bounds }
}

pub fn sponza_at(frame: u32) -> TriScene {
    let s = crate::scene::state_at(frame);
    sponza(s.box_step, s.l0_on, s.l2_on)
}

pub fn sponza_setup(w: usize, h: usize) -> TriSetup {
    // dans l'atrium (ouvert vers le ciel pour |z| < ~1,9 m), devant le mur du fond (x ≈ −12)
    let eye = v3(-10.0, 1.6, -0.3);
    let fwd = (v3(10.0, 4.0, -0.3) - eye).norm();
    let right = fwd.cross(v3(0.0, 1.0, 0.0)).norm();
    let tv = 0.5;
    TriSetup {
        name: format!("sponza_{w}x{h}"),
        view: crate::gpu_krc::View { w, h, eye, fwd, right, up: right.cross(fwd), th: tv * w as f64 / h as f64, tv },
        at: sponza_at,
        // petites lampes brillantes près des murs : lucioles à 4096 spp
        ref_spp: 16384,
        cam: None,
    }
}

/// Sponza, caméra mobile : on marche dans l'atrium (9 m en 120 frames, ~4,5 m/s à 60 Hz) en
/// regardant à gauche et à droite (lacet ±17°) et en ondulant latéralement.
pub fn sponza_cam_setup(w: usize, h: usize) -> TriSetup {
    fn cam(t: u32) -> (V3, V3) {
        let u = t as f64 / 119.0;
        let tau = 2.0 * std::f64::consts::PI;
        let eye = v3(-10.0 + 9.0 * u, 1.6, -0.3 + 0.8 * (tau * t as f64 / 120.0).sin());
        let yaw = 0.3 * (tau * t as f64 / 90.0).sin();
        (eye, eye + v3(yaw.cos(), 0.12, yaw.sin()) * 10.0)
    }
    TriSetup { name: format!("sponza_cam_{w}x{h}"), cam: Some(cam), ..sponza_setup(w, h) }
}

/// `krc sponzadebug` : ce que voit la caméra et où tombent les sondes de chaque lampe.
pub fn sponza_debug(gpu: &Gpu) {
    use crate::gpu_krc::GpuKrc;
    use crate::grid::Grid;
    let setup = sponza_setup(480, 270);
    let scene = sponza_at(0);
    println!("emprise {:?} -> {:?}, {} triangles", scene.bounds.min, scene.bounds.max, scene.triangles());
    let mut k = GpuKrc::with(gpu, setup.view, Grid::fit(&scene.bounds), Some(&scene), false, 1);
    let f = k.frame_tri(&scene, &scene, 0);
    let n = f.kinds.len() as f64;
    let count = |v: u8| f.kinds.iter().filter(|&&x| x == v).count() as f64 / n;
    println!("pixels : vides {:.2}, lampes {:.2}, surfaces {:.2}", count(0), count(1), count(2));
    let v = setup.view;
    let mut rays = Vec::new();
    for (sx, sy) in [(0.0, 0.0), (-0.9, 0.9), (0.9, 0.9), (-0.9, -0.9), (0.9, -0.9), (0.0, -0.9), (0.0, 0.9)] {
        rays.push((v.eye, (v.fwd + v.right * (sx * v.th) + v.up * (sy * v.tv)).norm()));
    }
    for ((_, d), h) in rays.iter().zip(k.trace_rays(&rays, 0)) {
        match h {
            Some((p, a)) => println!("rayon caméra {:?} : impact à {:.2} m en {:?}, albédo {:?}", d, (p - v.eye).len(), p, a),
            None => println!("rayon caméra {:?} : rien", d),
        }
    }
    for z in [-8.0, -6.0, -4.0, -2.0, -1.0, 0.0, 1.0, 2.0, 4.0, 6.0] {
        let eye = v3(-5.0, 1.8, z);
        let rays = vec![(eye, v3(0.0, 1.0, 0.0)), (eye, v3(0.0, -1.0, 0.0))];
        let h = k.trace_rays(&rays, 0);
        let d: Vec<String> = h.iter().map(|x| x.map_or("∞".to_string(), |(p, _)| format!("{:.2}", (p - eye).len()))).collect();
        println!("x=-5, z={z:>5} : au-dessus {}, au-dessous {}", d[0], d[1]);
    }
    for ex in [-11.5, -10.5, -9.5, -8.0] {
        let eye = v3(ex, 1.8, -0.3);
        let dirs = [v3(1.0, 0.1, 0.0), v3(1.0, 0.4, 0.4), v3(1.0, 0.4, -0.4), v3(1.0, -0.2, 0.4), v3(1.0, -0.2, -0.4), v3(0.0, 1.0, 0.0)];
        let rays: Vec<(V3, V3)> = dirs.iter().map(|d| (eye, d.norm())).collect();
        let h = k.trace_rays(&rays, 0);
        let dist: Vec<String> = h.iter().map(|x| x.map_or("∞".to_string(), |(p, _)| format!("{:.1}", (p - eye).len()))).collect();
        println!("œil x = {ex} : distances libres (avant, haut-gauche, haut-droite, bas-gauche, bas-droite, zénith) = {:?}", dist);
    }
    for (j, l) in scene.lights.iter().enumerate() {
        let hits = k.probes(l, 0, 7);
        let d: Vec<f64> = hits.iter().map(|(p, _)| (*p - l.center).len()).collect();
        let mean = d.iter().sum::<f64>() / d.len().max(1) as f64;
        let near = d.iter().filter(|&&x| x < 0.5).count();
        println!("lampe {j} en {:?} : {} sondes touchent, distance moyenne {:.2} m, {} à moins de 0,5 m", l.center, hits.len(), mean, near);
    }
}

/// `krc bias [cornell|sponza]` : luminance moyenne image / référence (Σ sur les pixels de surface),
/// pour séparer un biais d'un bruit. (a) scène finale statique depuis un cache vide,
/// (b) séquence complète, KRC et SHaRC.
pub fn bias(gpu: &Gpu, setup: &TriSetup) {
    use crate::gpu_krc::GpuKrc;
    use crate::grid::Grid;
    use crate::render::load_raw;
    use crate::scene::FRAMES;
    let (view, at) = (setup.view, setup.at);
    let grid = Grid::fit(&at(0).bounds);
    let dir = format!("results/ref_{}", setup.name);
    let refs: std::collections::HashMap<String, Vec<V3>> = (0..FRAMES)
        .map(|f| setup.ref_key(f))
        .map(|id| (id.clone(), load_raw(&format!("{dir}/{id}.bin"), view.len()).expect("références manquantes")))
        .collect();
    let ratio = |f: &crate::gpu_krc::GpuFrame, r: &[V3]| {
        let (mut a, mut b) = (0.0, 0.0);
        for i in 0..view.len() {
            if f.kinds[i] == 2 {
                a += f.image[i].lum();
                b += r[i].lum();
            }
        }
        a / b
    };
    let fs: u32 = std::env::var("KRC_BIAS_FRAME").ok().and_then(|s| s.parse().ok()).unwrap_or(FRAMES - 1);
    let last = at(fs);
    // (nom, sharc, plafond de variance, n_post, variance d'historique, plafond des cellules sans différence)
    let variants = [
        ("krc_avant", false, 0.0f32, 64.0f32, false, false),
        ("+plafond_var", false, 1.0, 64.0, false, false),
        ("+zero_cap", false, 1.0, 64.0, false, true),
        ("krc", false, 1.0, 16.0, true, true),
        ("sharc", true, 0.0, 0.0, false, false),
    ];
    for (name, sharc, cap, np, h, z) in variants {
        let mut k = GpuKrc::with(gpu, view, grid, Some(&last), sharc, 7);
        k.set_view(setup.view_at(fs));
        k.rel_cap = cap;
        k.n_post = np;
        k.s2_hist = h;
        k.zero_cap = z;
        let r = &refs[&setup.ref_key(fs)];
        let mut line = format!("(a) {name:<13} statique, état final :");
        for t in 0..200 {
            let f = k.frame_tri(&last, &last, t);
            if [0, 4, 9, 19, 39, 59, 99, 199].contains(&t) {
                line += &format!(" f{t} {:.3}", ratio(&f, r));
            }
        }
        println!("{line}");
    }
    for (name, sharc, cap, np, h, z) in variants {
        let mut k = GpuKrc::with(gpu, view, grid, Some(&at(0)), sharc, 7);
        k.rel_cap = cap;
        k.n_post = np;
        k.s2_hist = h;
        k.zero_cap = z;
        let mut prev = at(0);
        let mut line = format!("(b) {name:<13} séquence :");
        let mut err = 0.0;
        for t in 0..FRAMES {
            let scene = at(t);
            k.set_view(setup.view_at(t));
            let f = k.frame_tri(&scene, &prev, t);
            prev = scene;
            let (_, trim, _) = frame_err(&f.image, &f.kinds, &refs[&setup.ref_key(t)]);
            err += trim / FRAMES as f64;
            if [39, 63, 79, 80, 81, 85, 99, 100, 101, 105, 119].contains(&t) {
                line += &format!(" f{t} {:.3}", ratio(&f, &refs[&setup.ref_key(t)]));
            }
            if t == 80 || t == 100 {
                line += &format!(" [susp {} zéro {}]", f.suspects, f.delta_zero);
            }
        }
        println!("{line} | relMSE* {err:.3e}");
    }
}
